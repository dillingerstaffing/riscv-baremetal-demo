// satp_main.c: satp ASID-field write/readback and WARL discovery in S-mode.
//
// One mechanism: the supervisor address-translation and protection
// register (satp) on RV64. The register holds MODE in bits [63:60],
// ASID in bits [59:44], and PPN in bits [43:0]. The ASID field width
// (ASIDLEN) is WARL: writing all-ones to the ASID field and reading
// back exposes exactly which bits are implemented, which is ground
// truth for the machine's ASID width rather than an assumption.
//
// The run, in S-mode (dropped from M-mode boot via mret with
// mstatus.MPP=01; PMP is opened first because S-mode is default-deny
// with no PMP entry programmed, and an M-mode trap handler is
// installed so any unexpected trap is recorded and parked):
//   1. record the boot-time satp value (read in M-mode before the
//      drop; nothing writes satp between that read and the drop);
//   2. WARL discovery: csrw satp with MODE=0, PPN=0, ASID=all-ones;
//      read back, extract the ASID field, count the writable bits.
//      That count is the implemented ASIDLEN;
//   3. round-trip: for ASID in {0, 1, mid, max-writable} (exact
//      duplicates skipped when ASIDLEN is small), csrw satp then
//      csrr readback; print the write/readback/extracted-ASID
//      triple and PASS per value when the readback equals the
//      written value with unwritable ASID bits read back zero,
//      MODE reading back 0, and PPN reading back 0;
//   4. MODE WARL probe: zero satp, then write MODE=15 (a reserved
//      encoding) with ASID=0 and PPN=0; read back and check the
//      readback equals the pre-write value (the unsupported MODE
//      write takes no effect) and the MODE field is a legal
//      encoding (0 through 8). Translation is never enabled, so no
//      fault can occur;
//   5. check the trap counter is 0 (reaching the completion marker
//      proves no trap fired, since the handler parks on any trap),
//      print the completion marker and RESULT: PASS, then shut the
//      machine down via the virt test-device finisher (QEMU exits 0).
//      Any failed check prints RESULT: FAIL and parks instead.

#include "../uart.h"

extern void satp_trap_entry(void);

// Written by satp_trap.S if any trap fires (none expected).
volatile unsigned long satp_traps;
volatile unsigned long satp_mcause;
volatile unsigned long satp_mepc;
volatile unsigned long satp_mtval;
// Trap save area; mscratch points here while the module runs.
unsigned long satp_save[32];

#define MPP_MASK (3UL << 11)
#define MPP_S    (1UL << 11)

// satp field layout, RV64.
#define SATP_MODE_SHIFT 60
#define SATP_ASID_SHIFT 44
#define SATP_ASID_MASK  0xffffUL
#define SATP_MODE_MASK  (0xfUL << SATP_MODE_SHIFT)

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long csr_read_satp(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, satp" : "=r"(v));
    return v;
}

static void csr_write_satp(unsigned long v) {
    __asm__ volatile("csrw satp, %0" ::"r"(v) : "memory");
}

// Count the set bits in a 16-bit value: the number of writable ASID
// bits, i.e. the discovered ASIDLEN.
static unsigned popcount16(unsigned long v) {
    unsigned n = 0;
    while (v) {
        n += (unsigned)(v & 1UL);
        v >>= 1;
    }
    return n;
}

// The S-mode payload: the whole satp experiment runs here. Entered
// via mret with mstatus.MPP=01, so this function executes in S-mode.
// Global (not static) because the only reference is the `la` in the
// inline asm below, which the compiler cannot see.
void satp_smode_test(void) {
    uart_puts("in S-mode; satp experiment begins\n\n");

    // 2. WARL discovery: ASID all-ones, MODE=0, PPN=0.
    unsigned long disc_write = (SATP_ASID_MASK << SATP_ASID_SHIFT);
    csr_write_satp(disc_write);
    unsigned long disc_rb = csr_read_satp();
    unsigned long asid_writable =
        (disc_rb >> SATP_ASID_SHIFT) & SATP_ASID_MASK;
    unsigned asidlen = popcount16(asid_writable);
    unsigned long disc_mode = (disc_rb & SATP_MODE_MASK) >> SATP_MODE_SHIFT;
    unsigned long disc_ppn = disc_rb & ((1UL << SATP_ASID_SHIFT) - 1UL);

    uart_puts("WARL discovery:\n");
    uart_puts("  write            = ");
    uart_put_hex(disc_write);
    uart_puts("\n");
    uart_puts("  readback         = ");
    uart_put_hex(disc_rb);
    uart_puts("\n");
    uart_puts("  writable ASID mask = ");
    uart_put_hex(asid_writable);
    uart_puts("\n");
    uart_puts("  discovered ASIDLEN = ");
    uart_put_dec(asidlen);
    uart_puts("\n");
    uart_puts("  MODE field readback = ");
    uart_put_dec(disc_mode);
    uart_puts("\n");
    uart_puts("  PPN field readback  = ");
    uart_put_hex(disc_ppn);
    uart_puts("\n");
    check(disc_mode == 0, "MODE did not read back 0 after Bare write");
    check(disc_ppn == 0, "PPN did not read back 0 after zero write");
    // The writable mask must be a contiguous low run of bits; verify.
    check(asid_writable == (asidlen == 16 ? 0xffffUL
                                          : ((1UL << asidlen) - 1UL)),
          "writable ASID bits are not a contiguous low run");

    // 3. Round-trip: {0, 1, mid, max-writable}, duplicates skipped.
    unsigned long max_asid =
        (asidlen == 16) ? 0xffffUL : ((1UL << asidlen) - 1UL);
    unsigned long mid_asid = (asidlen >= 2) ? (1UL << (asidlen - 1)) : 0;
    unsigned long vals[4] = { 0, 1, mid_asid, max_asid };

    uart_puts("\nround-trip (MODE=0, PPN=0):\n");
    for (int i = 0; i < 4; i++) {
        unsigned long a = vals[i];
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (vals[j] == a)
                dup = 1;
        if (dup)
            continue;
        unsigned long w = (a << SATP_ASID_SHIFT);
        csr_write_satp(w);
        unsigned long rb = csr_read_satp();
        unsigned long got = (rb >> SATP_ASID_SHIFT) & SATP_ASID_MASK;
        // Expected: writable bits keep the written value, unwritable
        // ASID bits read back zero, MODE and PPN read back zero.
        unsigned long expect = ((a & max_asid) << SATP_ASID_SHIFT);
        int ok = (rb == expect);

        uart_puts("  ASID write = ");
        uart_put_hex(w);
        uart_puts(" readback = ");
        uart_put_hex(rb);
        uart_puts(" asid = ");
        uart_put_hex(got);
        uart_puts(ok ? "  PASS\n" : "  FAIL\n");
        if (!ok) {
            uart_puts("    expected readback = ");
            uart_put_hex(expect);
            uart_puts("\n");
            fails++;
        }
    }

    // 4. MODE WARL probe: reserved encoding 15, starting from satp=0
    // (ASID=0, PPN=0, MODE=0). The privileged spec allows an
    // unsupported MODE write to take no effect at all; on this
    // machine the whole write is ignored and the prior value is
    // preserved, which is exactly what the readback check pins down.
    // Translation is never enabled, so no fault can occur.
    csr_write_satp(0);
    check(csr_read_satp() == 0, "satp did not zero before MODE probe");
    unsigned long mode_write = (0xfUL << SATP_MODE_SHIFT);
    csr_write_satp(mode_write);
    unsigned long mode_rb = csr_read_satp();
    unsigned long got_mode =
        (mode_rb & SATP_MODE_MASK) >> SATP_MODE_SHIFT;
    unsigned long got_asid =
        (mode_rb >> SATP_ASID_SHIFT) & SATP_ASID_MASK;
    unsigned long got_ppn = mode_rb & ((1UL << SATP_ASID_SHIFT) - 1UL);

    uart_puts("\nMODE WARL probe (write MODE=15, reserved):\n");
    uart_puts("  write            = ");
    uart_put_hex(mode_write);
    uart_puts("\n");
    uart_puts("  readback         = ");
    uart_put_hex(mode_rb);
    uart_puts("\n");
    uart_puts("  MODE field       = ");
    uart_put_dec(got_mode);
    uart_puts("\n");
    uart_puts("  ASID field       = ");
    uart_put_hex(got_asid);
    uart_puts("\n");
    uart_puts("  PPN field        = ");
    uart_put_hex(got_ppn);
    uart_puts("\n");
    // Legal RV64 MODE encodings are 0 (Bare) through 8 (Sv57);
    // 9-15 are reserved, so a WARL field must read back 0-8. The
    // unsupported MODE=15 write must leave satp unchanged: the
    // readback must equal the pre-write value (0), which pins the
    // observed "write has no effect" behavior.
    check(got_mode <= 8, "MODE read back a reserved encoding");
    check(mode_rb == 0, "unsupported MODE write changed satp");
    check(got_asid == 0, "ASID changed during MODE probe");
    check(got_ppn == 0, "PPN changed during MODE probe");

    // Restore Bare with ASID 0.
    csr_write_satp(0);
    check(csr_read_satp() == 0, "satp did not restore to 0");

    // 5. Trap count and verdict. The M-mode handler parks on any
    // trap, so reaching this point with the counter at 0 proves the
    // happy path took no traps.
    uart_puts("\ntraps observed = ");
    uart_put_dec(satp_traps);
    uart_puts("\n");
    check(satp_traps == 0, "trap fired during the satp experiment");

    uart_puts("\nCOMPLETION MARKER: satp-asid run finished\n");
    if (fails == 0) {
        uart_puts("RESULT: PASS\n");
        __asm__ volatile("li t1, 0x100000\n\t"
                         "li t2, 0x5555\n\t"
                         "sw t2, 0(t1)\n\t"
                         :
                         :
                         : "t1", "t2", "memory");
        for (;;)
            __asm__ volatile("wfi"); // unreachable; safety net
    }
    uart_puts("RESULT: FAIL\n");
    for (;;)
        __asm__ volatile("wfi");
}

int main(void) {
    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("satp ASID write/readback + WARL discovery\n");
    uart_puts("========================================\n\n");

    // 1. Boot-time satp, read in M-mode before the drop.
    unsigned long boot_satp = csr_read_satp();
    uart_puts("satp at boot (M-mode read) = ");
    uart_put_hex(boot_satp);
    uart_puts("\n");

    // Install the M-mode trap handler; any trap from the S-mode
    // payload lands here, gets recorded, and parks.
    __asm__ volatile("la t0, satp_trap_entry\n\t"
                     "csrw mtvec, t0\n\t"
                     "la t0, satp_save\n\t"
                     "csrw mscratch, t0\n\t"
                     :
                     :
                     : "t0", "memory");

    // PMP: with no PMP entry programmed, S-mode has no access to
    // any address. Open the whole address space with one NAPOT
    // R/W/X entry before the drop; without this the first S-mode
    // instruction fetch raises an instruction access fault.
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t" // A=NAPOT, R/W/X, unlocked
                     "csrw pmpcfg0, t0\n\t"
                     :
                     :
                     : "t0", "memory");

    uart_puts("setup complete; dropping to S-mode...\n");

    // mret with MPP=01 (S-mode) into satp_smode_test. The whole
    // experiment runs in S-mode from there.
    __asm__ volatile("la t0, satp_smode_test\n\t"
                     "csrw mepc, t0\n\t"
                     "csrr t0, mstatus\n\t"
                     "li t1, 0x1800\n\t"   // clear MPP bits 12:11
                     "not t1, t1\n\t"
                     "and t0, t0, t1\n\t"
                     "li t1, 0x800\n\t"    // MPP = 01 (S-mode)
                     "or t0, t0, t1\n\t"
                     "csrw mstatus, t0\n\t"
                     "mret\n\t"
                     :
                     :
                     : "t0", "t1", "memory");

    // Unreachable: mret lands in the S-mode payload, which finishes
    // via the test-device finisher or parks on failure.
    for (;;)
        __asm__ volatile("wfi");
}
