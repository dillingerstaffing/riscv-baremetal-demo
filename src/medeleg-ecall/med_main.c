// med_main.c: medeleg bit-9 S-mode ecall delegation routing
// (backlog item 163).
//
// Mechanism under test: the medeleg CSR routes synchronous traps
// taken in S-mode. With medeleg bit 9 (supervisor environment
// call) set, an S-mode ecall must be delivered to the S-mode
// handler with scause = 9 and sepc at the ecall; with the bit
// clear, the same ecall must trap to M-mode with mcause = 9.
//
// The run, on the QEMU virt board:
//   M-mode boot:
//     1. Install direct-mode mtvec/stvec handlers, point mscratch
//        at med_regs and sscratch at st_regs.
//     2. Record the boot medeleg value; probe bit-9 writability by
//        writing 0x200 and requiring the readback to have bit 9
//        set, so the later delegation state is proven to take
//        effect rather than be silently ignored.
//     3. Open a whole-address-space PMP NAPOT entry (S-mode is
//        default-deny without one).
//     4. mret with MPP=01 into med_smode_phase1.
//   S-mode phase 1 (medeleg = 0x200): execute ecall at a labeled
//   4-byte site. Expected: exactly one S-mode trap with
//   scause = 9, sepc exactly at the ecall site, the handler's +4
//   advance landing on the labeled resume address,
//   sstatus.SPP = 1 (trap taken from S-mode), and the M-mode trap
//   count still 0. The payload then issues a deliberate illegal
//   instruction (.word 0); medeleg leaves cause 2 in M-mode, and
//   the M-mode handler treats it as the phase-complete signal and
//   jumps to med_phase2.
//   M-mode phase 2: require the M-mode trap count to be 1 (only
//   the phase-complete trap; the delegated ecall never reached
//   M-mode), clear medeleg and require the readback to be exactly
//   0, then mret with MPP=01 into med_smode_phase2.
//   S-mode phase 2 (medeleg = 0): execute ecall at a labeled site.
//   Expected: the M-mode handler records mcause = 9 with mepc at
//   the ecall site, the M-mode trap count advances to 2, and the
//   S-mode trap count is unchanged. The payload again issues the
//   illegal instruction; the M-mode handler jumps to med_finish.
//   M-mode med_finish: require M-mode trap count 3, phase-2 ecall
//   count 1, phase-complete count 2, S-mode trap count unchanged,
//   then print the FNV-1a checksum over all verdict values.
//
// Verdict: RESULT: PASS only if all 17 checks held. On PASS the
// virt test-device finisher word shuts the machine down (QEMU
// exits 0). On FAIL the hart parks in a wfi loop; the bench
// harness runs QEMU under timeout, so a FAIL is observable as
// exit status 124.
//
// The trap sites use in-asm numeric local labels (la t, 1f with
// 1: in the asm) so the assembler resolves the exact address;
// the toolchain miscompiles C &&label at -O2, so &&label is never
// used for trap-resume addresses.

#include "../uart.h"

extern void med_trap_entry(void);
extern void med_strap_entry(void);

// M-mode trap record, written by med_trap.S:
// [0] M-mode trap count, [1] mcause, [2] mepc,
// [3] phase-2 ecall mcause, [4] phase-2 ecall mepc,
// [5] phase-2 ecall count, [6] phase-complete count,
// [7] handler scratch for the interrupted t1.
volatile unsigned long med_regs[8];

// S-mode trap record, written by med_strap.S:
// [0] scause, [1] stval, [2] sepc at entry, [3] trap count,
// [4] sepc after the +4 advance, [5] interrupted t0,
// [6] sstatus.SPP at trap entry.
volatile unsigned long st_regs[7];

// Current phase: 1 while the delegated ecall is under test, 2
// while the non-delegated ecall is under test. Read by the M-mode
// handler to decide whether an mcause-9 trap is recorded and where
// a phase-complete trap resumes.
volatile unsigned long med_phase;

// S-mode trap count observed at the end of phase 1; phase 2 and
// the finish must see the same value (no new S-mode trap).
volatile unsigned long med_straps_p1;

// FNV-1a (64-bit) over the verdict-relevant values, fed in a
// fixed order from both M-mode setup and the S-mode payloads, so
// it is identical on every passing run.
static unsigned long long cksum = 1469598103934665603ULL;

static void cks_feed(unsigned long v) {
    int i;
    for (i = 0; i < 8; i++) {
        cksum ^= (unsigned long long)((v >> (8 * i)) & 0xffUL);
        cksum *= 1099511628211ULL;
    }
}

static void uart_put_hex64(unsigned long long v) {
    int i;
    uart_puts("0x");
    for (i = 15; i >= 0; i--) {
        unsigned int d = (unsigned int)((v >> (4 * i)) & 0xfULL);
        uart_putc(d < 10 ? (char)('0' + d) : (char)('a' + d - 10));
    }
}

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

#define MEDELEG_ECALL_S (1UL << 9)

static int fails = 0;
static int check_count = 0;

static void check(int cond, const char *msg) {
    check_count++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// The S-mode phase-1 payload. Entered via mret with
// mstatus.MPP=01. Global (not static) because the only reference
// is the `la` in the inline asm, which the compiler cannot see.
void med_smode_phase1(void) {
    unsigned long site, after;
    unsigned long scause, stval, sepc, sepc4, straps, spp, mtraps;
    int ok;

    uart_puts("in S-mode; phase 1: medeleg bit 9 set\n");
    uart_puts("(S-mode ecall must be delegated to S-mode)\n\n");

    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "la %1, 2f\n\t"
        "1: ecall\n\t"
        "2:\n\t"
        ".option pop\n\t"
        : "=r"(site), "=r"(after)
        :
        : "memory");

    scause = st_regs[0];
    stval = st_regs[1];
    sepc = st_regs[2];
    straps = st_regs[3];
    sepc4 = st_regs[4];
    spp = st_regs[6];
    mtraps = med_regs[0];

    uart_puts("phase 1 results:\n");
    uart_puts("  ecall site (label 1)  = ");
    uart_put_hex(site);
    uart_puts("\n  resume site (label 2) = ");
    uart_put_hex(after);
    uart_puts("\n  scause  = ");
    uart_put_hex(scause);
    uart_puts("\n  stval   = ");
    uart_put_hex(stval);
    uart_puts("\n  sepc    = ");
    uart_put_hex(sepc);
    uart_puts("\n  sepc+4  = ");
    uart_put_hex(sepc4);
    uart_puts("\n  S-mode traps  = ");
    uart_put_dec(straps);
    uart_puts("\n  sstatus.SPP   = ");
    uart_put_dec(spp);
    uart_puts("\n  M-mode traps  = ");
    uart_put_dec(mtraps);
    uart_puts("\n");

    ok = (straps == 1);
    check(ok, "phase 1: S-mode trap count is not 1");
    cks_feed(straps);
    ok = (scause == 9);
    check(ok, "phase 1: scause is not 9 (environment call from S-mode)");
    cks_feed(scause);
    ok = (sepc == site);
    check(ok, "phase 1: sepc is not at the ecall site");
    cks_feed(ok);
    ok = (sepc4 == after);
    check(ok, "phase 1: sepc+4 is not at the resume label");
    cks_feed(ok);
    ok = (spp == 1);
    check(ok, "phase 1: sstatus.SPP is not 1 (trap not taken from S-mode)");
    cks_feed(spp);
    ok = (mtraps == 0);
    check(ok, "phase 1: M-mode saw a trap for the delegated ecall");
    cks_feed(mtraps);

    med_straps_p1 = straps;

    // Hand control back to M-mode. This deliberate illegal
    // instruction (.word 0) traps with mcause 2, which medeleg
    // leaves in M-mode; the M-mode handler treats it as the
    // phase-complete signal and jumps to med_phase2. Unreachable
    // past the trap.
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        ".word 0\n\t"
        ".option pop\n\t"
        :
        :
        : "memory");
    for (;;)
        __asm__ volatile("wfi");
}

// M-mode phase-2 entry. Reached from the M-mode trap handler when
// the phase-1 payload issues its phase-complete illegal
// instruction.
void med_phase2(void) {
    unsigned long mtraps, rb;
    int ok;

    uart_puts("\nphase 2: medeleg bit 9 clear\n");
    uart_puts("(S-mode ecall must trap to M-mode)\n\n");

    mtraps = med_regs[0];
    uart_puts("  M-mode traps after phase 1 = ");
    uart_put_dec(mtraps);
    uart_puts("\n");
    ok = (mtraps == 1);
    check(ok, "phase 2: M-mode trap count is not 1 (the delegated ecall reached M-mode)");
    cks_feed(mtraps);

    // Clear the delegation. The readback must be exactly 0 so the
    // cleared state is proven to take effect rather than be
    // silently ignored.
    __asm__ volatile("csrw medeleg, zero" ::: "memory");
    __asm__ volatile("csrr %0, medeleg" : "=r"(rb));
    uart_puts("  medeleg after clear = ");
    uart_put_hex(rb);
    uart_puts("\n");
    ok = (rb == 0);
    check(ok, "phase 2: medeleg did not read back 0 after clear");
    cks_feed(rb);

    uart_puts("dropping to S-mode for phase 2...\n");

    // mret with MPP=01 (S-mode) into med_smode_phase2.
    med_phase = 2;
    __asm__ volatile("la t0, med_smode_phase2\n\t"
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

    // Unreachable: mret lands in the S-mode payload.
    for (;;)
        __asm__ volatile("wfi");
}

// The S-mode phase-2 payload. Entered via mret with
// mstatus.MPP=01 from med_phase2.
void med_smode_phase2(void) {
    unsigned long site2, after2;
    unsigned long mtraps, mcause, mepc, straps;
    int ok;

    uart_puts("in S-mode; phase 2 ecall (expect M-mode trap):\n");

    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "la %1, 2f\n\t"
        "1: ecall\n\t"
        "2:\n\t"
        ".option pop\n\t"
        : "=r"(site2), "=r"(after2)
        :
        : "memory");

    mtraps = med_regs[0];
    mcause = med_regs[3];
    mepc = med_regs[4];
    straps = st_regs[3];

    uart_puts("phase 2 results:\n");
    uart_puts("  ecall site (label 1) = ");
    uart_put_hex(site2);
    uart_puts("\n  M-mode traps  = ");
    uart_put_dec(mtraps);
    uart_puts("\n  mcause (ecall) = ");
    uart_put_hex(mcause);
    uart_puts("\n  mepc (ecall)   = ");
    uart_put_hex(mepc);
    uart_puts("\n  S-mode traps  = ");
    uart_put_dec(straps);
    uart_puts(" (phase 1 ended at ");
    uart_put_dec(med_straps_p1);
    uart_puts(")\n");

    ok = (mtraps == 2);
    check(ok, "phase 2: M-mode trap count is not 2");
    cks_feed(mtraps);
    ok = (mcause == 9);
    check(ok, "phase 2: mcause is not 9 (environment call from S-mode)");
    cks_feed(mcause);
    ok = (mepc == site2);
    check(ok, "phase 2: mepc is not at the ecall site");
    cks_feed(ok);
    ok = (straps == med_straps_p1);
    check(ok, "phase 2: unexpected S-mode trap fired");
    cks_feed(straps);

    // Hand back to M-mode for the verdict: the same deliberate
    // illegal instruction; the handler jumps to med_finish.
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        ".word 0\n\t"
        ".option pop\n\t"
        :
        :
        : "memory");
    for (;;)
        __asm__ volatile("wfi");
}

// M-mode verdict. Reached from the M-mode trap handler when the
// phase-2 payload issues its phase-complete illegal instruction.
void med_finish(void) {
    unsigned long mtraps, ecount, pcount, straps;
    int ok;

    mtraps = med_regs[0];
    ecount = med_regs[5];
    pcount = med_regs[6];
    straps = st_regs[3];

    uart_puts("\nfinal trap counts:\n");
    uart_puts("  M-mode traps   = ");
    uart_put_dec(mtraps);
    uart_puts("\n  phase-2 ecalls = ");
    uart_put_dec(ecount);
    uart_puts("\n  phase-completes= ");
    uart_put_dec(pcount);
    uart_puts("\n  S-mode traps   = ");
    uart_put_dec(straps);
    uart_puts("\n");

    ok = (mtraps == 3);
    check(ok, "finish: M-mode trap count is not 3");
    cks_feed(mtraps);
    ok = (ecount == 1);
    check(ok, "finish: phase-2 ecall count is not 1");
    cks_feed(ecount);
    ok = (pcount == 2);
    check(ok, "finish: phase-complete count is not 2");
    cks_feed(pcount);
    ok = (straps == med_straps_p1);
    check(ok, "finish: S-mode trap count changed after phase 2");
    cks_feed(straps);

    uart_puts("\nchecksum (FNV-1a over verdict values) = ");
    uart_put_hex64(cksum);
    uart_puts("\n");
    uart_puts("checks=");
    uart_put_dec((unsigned long)check_count);
    uart_puts(" mismatches=");
    uart_put_dec((unsigned long)fails);
    uart_puts("\n");

    uart_puts(fails == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;
    }
    for (;;) {
        __asm__ volatile("wfi");
    }
}

int main(void) {
    unsigned long boot_md, rb;
    int ok;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("medeleg-ecall: S-mode ecall delegation routing\n");
    uart_puts("bit 9 set: S-mode handles, bit 9 clear: M-mode handles\n");
    uart_puts("========================================\n\n");

    // Trap vectors and handler scratch areas.
    __asm__ volatile("la t0, med_trap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, med_regs\n\t"
                     "csrw mscratch, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, med_strap_entry\n\t"
                     "csrw stvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, st_regs\n\t"
                     "csrw sscratch, t0"
                     :
                     :
                     : "t0", "memory");

    // Boot medeleg and the bit-9 writability probe: write 0x200,
    // read back; the readback must have bit 9 set so the later
    // delegation state is proven to take effect rather than be
    // silently ignored (a lone clear would be ambiguous if the
    // bit were read-only-zero).
    __asm__ volatile("csrr %0, medeleg" : "=r"(boot_md));
    uart_puts("boot: medeleg=");
    uart_put_hex(boot_md);
    uart_puts("\n");

    __asm__ volatile("li t0, 0x200\n\t"
                     "csrw medeleg, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("csrr %0, medeleg" : "=r"(rb));
    uart_puts("probe: csrw medeleg, 0x200; readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    ok = ((rb & MEDELEG_ECALL_S) != 0);
    check(ok, "medeleg bit 9 did not take the write");
    cks_feed(rb);

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

    uart_puts("setup complete; dropping to S-mode for phase 1...\n");

    // mret with MPP=01 (S-mode) into med_smode_phase1. Phase 1
    // runs with medeleg bit 9 set from the probe above.
    med_phase = 1;
    __asm__ volatile("la t0, med_smode_phase1\n\t"
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

    // Unreachable: mret lands in the S-mode payload, which returns
    // to M-mode through the trap handler and finishes via the
    // test-device finisher or parks on failure.
    for (;;)
        __asm__ volatile("wfi");
}
