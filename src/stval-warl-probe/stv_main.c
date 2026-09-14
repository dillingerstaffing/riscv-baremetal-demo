// stv_main.c: stval WARL write probe (backlog item "riscv stval-warl-probe").
//
// Mechanism under test: stval is a WARL CSR, so a write of
// all-ones must legalize to whatever value the hart permits,
// and a write of zero must read back zero. The probe runs in
// S-mode on the QEMU virt board and publishes the legalized
// readbacks: all-ones (expected to be stored verbatim on this
// hart, readback 0xffffffffffffffff), zero (readback 0x0), and a
// restore of the boot value. Between the writes the module takes
// one deliberate illegal-instruction trap (csrr t0, 0x7ff, an
// unimplemented CSR) so the run also records what the hart
// writes into stval on a trap: the S-mode handler captures
// scause/stval/sepc, and the verdict requires scause == 2,
// sepc exactly at the fault site, stval exactly equal to the
// faulting instruction word read back from the site address,
// and the destination register still holding its pre-fault
// sentinel.
//
// The run, on the QEMU virt board:
//   M-mode boot:
//     1. Record the boot stval value (QEMU resets it to 0) and
//        publish it.
//     2. Install the M-mode mtvec handler (parks the hart on any
//        unexpected M-mode trap; on the one expected S-mode ecall
//        it records mcause/mepc and the trap count, advances mepc
//        past the ecall, and calls the C verdict routine, which
//        prints the checksum and RESULT and then either shuts the
//        machine down through the virt test-device finisher on
//        PASS or parks in wfi on FAIL), the S-mode stvec handler
//        (records scause/stval/sepc, bumps the trap counter,
//        advances sepc by 4, sret), point mscratch at the M-mode
//        record and sscratch at the S-mode record, set medeleg =
//        0x4 so the illegal-instruction trap is delivered to
//        S-mode while the final ecall stays in M-mode, and open
//        a whole-address-space PMP NAPOT entry (S-mode is
//        default-deny without one).
//     3. mret with MPP=01 into stv_smode_test.
//   S-mode payload:
//     1. csrr stval; record the boot readback (expect 0x0).
//     2. csrw stval, all-ones; csrr readback (expect the
//        verbatim value 0xffffffffffffffff on this hart).
//     3. csrw stval, 0x0; csrr readback (expect 0x0).
//     4. Execute `csrr t0, 0x7ff` at a labeled 4-byte site with
//        t0 preloaded with a sentinel. Expected: exactly one
//        S-mode trap with scause = 2, sepc exactly at the fault
//        site, the handler's +4 advance landing on the labeled
//        resume address, stval exactly equal to the faulting
//        instruction word read from the site address, and t0
//        still holding the sentinel (the instruction never
//        retired).
//     5. csrw stval, boot value; csrr readback (expect the boot
//        value).
//     6. ecall back to M-mode. Expected: exactly one M-mode
//        trap with mcause = 9 and mepc exactly at the ecall
//        site.
//   Verdict: RESULT: PASS only if all 14 checks held. On PASS
//   the virt test-device finisher word shuts the machine down
//   (QEMU exits 0). On FAIL the hart parks in a wfi loop; the
//   bench harness runs QEMU under timeout, so a FAIL is
//   observable as exit status 124.
//
// The fault sites use in-asm numeric local labels (la t, 1f with
// 1: in the asm) so the assembler resolves the exact address;
// the toolchain miscompiles C &&label at -O2, so &&label is never
// used for trap-resume addresses.

#include "../uart.h"

extern void stv_strap_entry(void);
extern void stv_mtrap_entry(void);

// S-mode trap record, written by stv_strap.S:
// [0] scause, [1] stval, [2] sepc at entry, [3] trap count,
// [4] sepc after the +4 advance, [5] interrupted t0.
volatile unsigned long stv_st_regs[6];

// M-mode trap record, written by stv_mtrap.S:
// [0] mcause, [1] mepc at entry, [2] M-mode trap count.
volatile unsigned long stv_m_regs[3];

// Ecall site label address, captured in S-mode before the ecall,
// checked by the M-mode verdict routine.
unsigned long stv_ecall_site;

// FNV-1a (64-bit) over the verdict-relevant values, fed in a fixed
// order from both privilege levels, so it is identical on every
// passing run. Every fed value is deterministic; no timing values
// are fed.
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

#define SENTINEL 0xDEADBEEFDEADBEEFUL
#define MEDELEG_ILLEGAL_INSN (1UL << 2)
#define MCAUSE_ECALL_SMODE 9UL

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// M-mode verdict routine, called once from stv_mtrap.S on the
// expected S-mode ecall. Performs the M-mode checks, prints the
// checksum and RESULT, then either shuts the machine down through
// the finisher (PASS) or parks the hart in wfi (FAIL). Never
// returns.
void stv_ecall_done(void) {
    unsigned long mcause = stv_m_regs[0];
    unsigned long mepc = stv_m_regs[1];
    unsigned long mtraps = stv_m_regs[2];
    int ok;

    uart_puts("\nM-mode: ecall received from S-mode:\n");
    uart_puts("  ecall site (label 1) = ");
    uart_put_hex(stv_ecall_site);
    uart_puts("\n  mcause  = ");
    uart_put_hex(mcause);
    uart_puts("\n  mepc    = ");
    uart_put_hex(mepc);
    uart_puts("\n  M-mode traps = ");
    uart_put_dec(mtraps);
    uart_puts("\n");

    ok = (mtraps == 1);
    check(ok, "M-mode trap count is not 1");
    cks_feed(mtraps);
    ok = (mcause == MCAUSE_ECALL_SMODE);
    check(ok, "mcause is not 9 (ecall from S-mode)");
    cks_feed(mcause);
    ok = (mepc == stv_ecall_site);
    check(ok, "mepc is not at the ecall site");
    cks_feed(ok);

    uart_puts("\nchecksum (FNV-1a over verdict values) = ");
    uart_put_hex64(cksum);
    uart_puts("\n");

    uart_puts(fails == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;
    }
    for (;;) {
        __asm__ volatile("wfi");
    }
}

// The S-mode payload. Entered via mret with mstatus.MPP=01.
// Global (not static) because the only reference is the `la` in
// main's inline asm, which the compiler cannot see.
void stv_smode_test(void) {
    unsigned long boot_s, ones_rb, zero_rb, rb;
    unsigned long site, after, t0_after, enc;
    unsigned long scause, stval, sepc, sepc_after, traps;
    unsigned long sent = SENTINEL;
    int ok;

    uart_puts("in S-mode; stval WARL probe begins\n\n");

    // Phase 1: record the boot stval readback (expect 0x0).
    __asm__ volatile("csrr %0, stval" : "=r"(boot_s));
    uart_puts("phase 1: boot stval readback = ");
    uart_put_hex(boot_s);
    uart_puts("\n");
    ok = (boot_s == 0x0UL);
    check(ok, "phase 1: boot stval is not 0x0");
    cks_feed(boot_s);

    // Phase 2: write all-ones, read back the legalized value.
    // On this hart the write is stored verbatim.
    __asm__ volatile("csrw stval, %0" ::"r"(0xFFFFFFFFFFFFFFFFUL));
    __asm__ volatile("csrr %0, stval" : "=r"(ones_rb));
    uart_puts("phase 2: csrw stval, 0xffffffffffffffff; readback = ");
    uart_put_hex(ones_rb);
    uart_puts("\n");
    ok = (ones_rb == 0xFFFFFFFFFFFFFFFFUL);
    check(ok, "phase 2: all-ones write did not read back verbatim");
    cks_feed(ones_rb);

    // Phase 3: write zero, read back.
    __asm__ volatile("csrw stval, %0" ::"r"(0x0UL));
    __asm__ volatile("csrr %0, stval" : "=r"(zero_rb));
    uart_puts("phase 3: csrw stval, 0x0; readback = ");
    uart_put_hex(zero_rb);
    uart_puts("\n");
    ok = (zero_rb == 0x0UL);
    check(ok, "phase 3: zero write did not read back as 0x0");
    cks_feed(zero_rb);

    // Phase 4: deliberate illegal-instruction trap. `csrr t0,
    // 0x7ff` reads an unimplemented CSR; t0 is preloaded with the
    // sentinel and the handler preserves the interrupted t0 via
    // the sscratch swap, so a post-trap t0 equal to the sentinel
    // proves the faulting instruction never retired. The fault
    // site address is taken with an in-asm numeric local label
    // so the assembler resolves it exactly. The trap handler
    // clobbers t1, so t1 is listed as clobbered here.
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "la %1, 2f\n\t"
        "mv t0, %3\n\t"
        "1: csrr t0, 0x7ff\n\t"
        "2: mv %2, t0\n\t"
        ".option pop\n\t"
        : "=r"(site), "=r"(after), "=r"(t0_after)
        : "r"(sent)
        : "t0", "t1", "memory");

    scause = stv_st_regs[0];
    stval = stv_st_regs[1];
    sepc = stv_st_regs[2];
    traps = stv_st_regs[3];
    sepc_after = stv_st_regs[4];

    // The faulting instruction word, read from the site address.
    enc = *(volatile unsigned int *)site;

    uart_puts("\nphase 4: illegal CSR read at labeled site (expect S-mode trap):\n");
    uart_puts("  fault site (label 1)  = ");
    uart_put_hex(site);
    uart_puts("\n  resume site (label 2) = ");
    uart_put_hex(after);
    uart_puts("\n  faulting instruction word = ");
    uart_put_hex(enc);
    uart_puts("\n  scause  = ");
    uart_put_hex(scause);
    uart_puts("\n  stval   = ");
    uart_put_hex(stval);
    uart_puts("\n  sepc    = ");
    uart_put_hex(sepc);
    uart_puts("\n  sepc+4  = ");
    uart_put_hex(sepc_after);
    uart_puts("\n  traps   = ");
    uart_put_dec(traps);
    uart_puts("\n  t0 after trap = ");
    uart_put_hex(t0_after);
    uart_puts(" (sentinel = ");
    uart_put_hex(SENTINEL);
    uart_puts(")\n");

    ok = (traps == 1);
    check(ok, "phase 4: trap count is not 1");
    cks_feed(traps);
    ok = (scause == 2);
    check(ok, "phase 4: scause is not 2 (illegal instruction)");
    cks_feed(scause);
    ok = (sepc == site);
    check(ok, "phase 4: sepc is not at the fault site");
    cks_feed(ok);
    ok = (sepc_after == after);
    check(ok, "phase 4: sepc+4 is not at the resume label");
    cks_feed(ok);
    ok = (stval == enc);
    check(ok, "phase 4: stval is not the faulting instruction word");
    cks_feed(stval);
    cks_feed(ok);
    ok = (t0_after == SENTINEL);
    check(ok, "phase 4: t0 changed (the faulting insn retired?)");
    cks_feed(t0_after);

    // Phase 5: restore stval to the boot value, read back.
    __asm__ volatile("csrw stval, %0" ::"r"(boot_s));
    __asm__ volatile("csrr %0, stval" : "=r"(rb));
    uart_puts("\nphase 5: csrw stval, boot value; readback = ");
    uart_put_hex(rb);
    uart_puts("\n");
    ok = (rb == boot_s);
    check(ok, "phase 5: restore did not read back the boot value");
    cks_feed(rb);

    // Phase 6: ecall back to M-mode for the verdict. The ecall
    // site address is stored to the stv_ecall_site global inside
    // the asm, before the ecall executes: the M-mode handler
    // never returns, so a compiler-scheduled store after the asm
    // would never run. The trap handler clobbers t1, so t1 is
    // listed as clobbered here.
    uart_puts("\nphase 6: ecall to M-mode for the verdict...\n");
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la t0, 1f\n\t"
        "la t1, stv_ecall_site\n\t"
        "sd t0, 0(t1)\n\t"
        "1: ecall\n\t"
        ".option pop\n\t"
        :
        :
        : "t0", "t1", "memory");

    for (;;) {
        __asm__ volatile("wfi");
    }
}

int main(void) {
    unsigned long boot_stval, deleg;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("stval: WARL write probe in S-mode\n");
    uart_puts("all-ones / zero legalized readbacks, plus the\n");
    uart_puts("trap-time stval capture on an illegal CSR read\n");
    uart_puts("========================================\n\n");

    // 1. Record the boot stval value (QEMU resets it to 0).
    __asm__ volatile("csrr %0, stval" : "=r"(boot_stval));
    uart_puts("boot: stval=");
    uart_put_hex(boot_stval);
    uart_puts("\n");

    // 2. Trap vectors, delegation, PMP.
    __asm__ volatile("la t0, stv_mtrap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, stv_strap_entry\n\t"
                     "csrw stvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, stv_m_regs\n\t"
                     "csrw mscratch, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, stv_st_regs\n\t"
                     "csrw sscratch, t0"
                     :
                     :
                     : "t0", "memory");
    // Delegate only the illegal-instruction trap (bit 2) to
    // S-mode; the final ecall stays in M-mode so the handler can
    // print the verdict. A correct run takes no other M-mode
    // trap.
    __asm__ volatile("li t0, 4\n\t"
                     "csrw medeleg, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("csrr %0, medeleg" : "=r"(deleg));
    uart_puts("medeleg readback=");
    uart_put_hex(deleg);
    uart_puts("\n");
    check((deleg & MEDELEG_ILLEGAL_INSN) != 0,
          "medeleg bit 2 (illegal instruction) not set");
    cks_feed((deleg & MEDELEG_ILLEGAL_INSN) != 0);

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

    // 3. mret with MPP=01 (S-mode) into stv_smode_test. The whole
    // experiment runs in S-mode from there, with one M-mode
    // excursion via ecall for the verdict.
    __asm__ volatile("la t0, stv_smode_test\n\t"
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

    // Unreachable: mret lands in the S-mode payload, which
    // finishes via the test-device finisher or parks on failure.
    for (;;)
        __asm__ volatile("wfi");
}
