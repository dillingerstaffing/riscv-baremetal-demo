// scw_main.c: scause WARL (write-any/read-legal) check.
//
// Mechanism under test: the scause CSR is write-any/read-legal
// (WARL): a software write is always accepted and the readback
// is the value the hart permits. Trap entry overwrites scause
// with the real trap cause regardless of what software wrote
// last. This is the S-mode cause register; the shipped
// src/mcause-warl module probed the M-mode cause register.
//
// Sequence under test (QEMU 8.2.2, virt machine):
//   M-mode boot:
//     1. Record the boot scause/mideleg baselines. Install
//        direct-mode mtvec (mscratch scratch area) and
//        direct-mode stvec (sscratch scratch area).
//     2. Phase 1: csrw all-ones to scause and publish the
//        legalized readback (require: no trap fired, the
//        readback is a fixed point of a second identical
//        write); csrw zero and require the readback to be zero
//        (no bits are read-only), with no trap fired by either
//        write.
//     3. Phase 2: delegate supervisor environment calls
//        (medeleg bit 9), require the bit to take via readback,
//        write all-ones to scause one last time (the last
//        software write before the trap), open the address
//        space with one PMP NAPOT entry, and mret with MPP=01
//        into the S-mode payload.
//   S-mode payload:
//     4. Execute ecall at a labeled 4-byte site. The S-mode
//        handler records scause (expect 9), stval, sepc (expect
//        exactly the ecall site), and sstatus.SPP (expect 1),
//        bumps the S-mode trap count, and resumes past the
//        ecall. The M-mode trap count must still be 0: the
//        delegated ecall never reached M-mode.
//     5. Hand back to M-mode with a deliberate illegal
//        instruction (.word 0). medeleg leaves cause 2 in
//        M-mode; the M-mode handler treats it as the
//        phase-complete signal and jumps to scw_finish.
//   M-mode scw_finish:
//     6. Require the M-mode trap count to be 1 (only the
//        phase-complete trap), scause to still read 9 (the
//        M-mode trap entry did not overwrite the S-mode cause
//        register), and restore mideleg to its boot value.
//
// Verdict: RESULT: PASS only if every check held. The
// verdict-relevant values feed a 64-bit FNV-1a digest printed as
// the last data line, so the three bench runs can be compared
// for byte-identical output. On PASS the machine is shut down
// through the virt test-device finisher (QEMU exits 0); on FAIL
// the hart parks without touching the finisher.
//
// The trap sites use in-asm numeric local labels (la t, 1f with
// 1: in the asm) so the assembler resolves the exact address;
// C &&label is never used for trap-resume addresses.

#include "../uart.h"

extern void scw_trap_entry(void);
extern void scw_strap_entry(void);

// M-mode trap record, written by scw_trap.S:
// [0] trap count, [1] mcause, [2] mepc, [3..6] unused,
// [7] handler scratch for the interrupted t1.
volatile unsigned long scw_regs[8];

// S-mode trap record, written by scw_strap.S:
// [0] scause, [1] stval, [2] sepc at entry, [3] trap count,
// [4] sepc after the +4 advance, [5] interrupted t0,
// [6] sstatus.SPP at trap entry.
volatile unsigned long st_regs[7];

// Boot mideleg, recorded in main and used by scw_finish to
// restore the delegation state.
static unsigned long boot_mideleg;

// FNV-1a (64-bit) over the verdict-relevant values, fed in a
// fixed order, so it is identical on every passing run.
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

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final RESULT line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

#define MEDELEG_ECALL_S (1UL << 9)
#define SCAUSE_ALL (~0UL)
#define ECALL_SCAUSE 9UL

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

static unsigned long read_scause(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, scause" : "=r"(v));
    return v;
}

static void write_scause(unsigned long v) {
    __asm__ volatile("csrw scause, %0" : : "r"(v));
}

// The S-mode payload. Entered via mret with mstatus.MPP=01.
// Global (not static) because the only reference is the `la` in
// the inline asm, which the compiler cannot see.
void scw_smode(void) {
    unsigned long site, after;
    unsigned long cause, stval, sepc, sepc4, straps, spp, mtraps;
    int ok;

    uart_puts("in S-mode; ecall at a labeled site\n");
    uart_puts("(medeleg bit 9 set: the trap must stay in S-mode)\n\n");

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

    cause = st_regs[0];
    stval = st_regs[1];
    sepc = st_regs[2];
    straps = st_regs[3];
    sepc4 = st_regs[4];
    spp = st_regs[6];
    mtraps = scw_regs[0];

    uart_puts("S-mode trap results:\n");
    uart_puts("  ecall site (label 1)  = ");
    uart_put_hex(site);
    uart_puts("\n  resume site (label 2) = ");
    uart_put_hex(after);
    uart_puts("\n  scause  = ");
    uart_put_hex(cause);
    uart_puts(" (expect 0x9)\n  stval   = ");
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
    uart_puts(" (expect 0: the delegated ecall never reached M-mode)\n");

    ok = (straps == 1);
    check(ok, "S-mode trap count is not 1");
    cks_feed(straps);
    ok = (cause == ECALL_SCAUSE);
    check(ok, "scause is not 9 (environment call from S-mode)");
    cks_feed(cause);
    ok = (sepc == site);
    check(ok, "sepc is not at the ecall site");
    cks_feed(ok);
    ok = (sepc4 == after);
    check(ok, "sepc+4 is not at the resume label");
    cks_feed(ok);
    ok = (spp == 1);
    check(ok, "sstatus.SPP is not 1 (trap not taken from S-mode)");
    cks_feed(spp);
    ok = (stval == 0);
    check(ok, "stval is not 0 for the ecall");
    cks_feed(stval);
    ok = (mtraps == 0);
    check(ok, "M-mode saw a trap for the delegated ecall");
    cks_feed(mtraps);

    // Hand control back to M-mode. This deliberate illegal
    // instruction (.word 0) traps with mcause 2, which medeleg
    // leaves in M-mode; the M-mode handler treats it as the
    // phase-complete signal and jumps to scw_finish. Unreachable
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

// M-mode verdict. Reached from the M-mode trap handler when the
// S-mode payload issues its phase-complete illegal instruction.
// Global (not static): the only reference is the `la` in
// scw_trap.S, which the compiler cannot see.
void scw_finish(void) {
    unsigned long mtraps, v, rb;
    int ok;

    mtraps = scw_regs[0];
    uart_puts("\nfinish (M-mode):\n");
    uart_puts("  M-mode traps = ");
    uart_put_dec(mtraps);
    uart_puts(" (expect 1: only the phase-complete trap)\n");
    ok = (mtraps == 1);
    check(ok, "M-mode trap count is not 1");
    cks_feed(mtraps);

    // The M-mode phase-complete trap must not have overwritten
    // scause: it still holds the S-mode trap cause.
    v = read_scause();
    uart_puts("  scause at finish = ");
    uart_put_hex(v);
    uart_puts(" (expect 0x9)\n");
    ok = (v == ECALL_SCAUSE);
    check(ok, "scause no longer holds the S-mode trap cause");
    cks_feed(v);

    // Restore mideleg to the boot value so the run leaves the
    // delegation state as it found it.
    __asm__ volatile("csrw medeleg, %0" : : "r"(boot_mideleg) : "memory");
    __asm__ volatile("csrr %0, medeleg" : "=r"(rb));
    uart_puts("  mideleg restored = ");
    uart_put_hex(rb);
    uart_puts("\n");
    ok = (rb == boot_mideleg);
    check(ok, "mideleg did not restore to the boot value");
    cks_feed(rb);

    uart_puts("\nchecksum (FNV-1a over verdict values) = ");
    uart_put_hex64(cksum);
    uart_puts("\n");
    uart_puts("checks=");
    uart_put_dec((unsigned long)check_count);
    uart_puts(" mismatches=");
    uart_put_dec((unsigned long)fails);
    uart_puts("\n");

    uart_puts(fails == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");

    // Let the UART drain (TEMT: transmitter fully empty) before
    // touching the finisher device.
    while ((*UART0_LSR & LSR_TEMT) == 0)
        ;

    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    // FAIL: park the hart without touching the finisher device.
    // The harness runs QEMU under timeout, so a FAIL is
    // observable as the timeout exit status (124) as well as the
    // RESULT line.
    for (;;)
        __asm__ volatile("wfi");
}

int main(void) {
    unsigned long v, rb, tv;
    int ok;

    uart_init();
    uart_puts("scw: scause WARL check\n");
    uart_puts("(S-mode cause register; cf. mcause-warl for M-mode)\n\n");

    // Boot baselines: scause and mideleg before anything runs.
    v = read_scause();
    uart_puts("boot: scause=");
    uart_put_hex(v);
    uart_puts("\n");
    ok = (v == 0);
    check(ok, "scause nonzero at boot");
    cks_feed(v);

    __asm__ volatile("csrr %0, mideleg" : "=r"(boot_mideleg));
    uart_puts("boot: mideleg=");
    uart_put_hex(boot_mideleg);
    uart_puts("\n");
    ok = ((boot_mideleg & MEDELEG_ECALL_S) == 0);
    check(ok, "medeleg bit 9 already set at boot");
    cks_feed(boot_mideleg);

    // Install the trap handlers: direct-mode mtvec with mscratch
    // pointing at the M-mode scratch area, direct-mode stvec with
    // sscratch pointing at the S-mode scratch area. No interrupt
    // source is armed, so only the deliberate traps can fire.
    __asm__ volatile("la t0, scw_trap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, scw_regs\n\t"
                     "csrw mscratch, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, scw_strap_entry\n\t"
                     "csrw stvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, st_regs\n\t"
                     "csrw sscratch, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
    uart_puts("trap: mtvec=");
    uart_put_hex(tv);
    uart_puts("\n");
    ok = ((tv & ~3UL) == (unsigned long)scw_trap_entry);
    check(ok, "mtvec did not take the handler address");
    ok = ((tv & 3UL) == 0);
    check(ok, "mtvec not in direct mode");
    __asm__ volatile("csrr %0, stvec" : "=r"(tv));
    uart_puts("trap: stvec=");
    uart_put_hex(tv);
    uart_puts("\n");
    ok = ((tv & ~3UL) == (unsigned long)scw_strap_entry);
    check(ok, "stvec did not take the handler address");
    ok = ((tv & 3UL) == 0);
    check(ok, "stvec not in direct mode");

    // Phase 1: WARL probes from M-mode (scs is M-mode-accessible).
    // Write all-ones, publish the legalized readback; the write
    // must take, the readback must be a fixed point of a second
    // identical write, and no trap may fire from the write
    // itself.
    uart_puts("\nphase 1: WARL probes\n");
    write_scause(SCAUSE_ALL);
    v = read_scause();
    uart_puts("  write all-ones; readback=");
    uart_put_hex(v);
    uart_puts("\n");
    ok = (scw_regs[0] == 0);
    check(ok, "trap fired from the all-ones write");
    ok = (v == SCAUSE_ALL);
    check(ok, "scause readback != all-ones after write");
    cks_feed(v);
    write_scause(v);
    rb = read_scause();
    ok = (rb == v);
    check(ok, "scause readback not a fixed point of the write");
    cks_feed(rb);

    // Write zero: the readback must be zero (no bits read-only),
    // and again no trap may fire.
    write_scause(0UL);
    v = read_scause();
    uart_puts("  write zero; readback=");
    uart_put_hex(v);
    uart_puts("\n");
    ok = (scw_regs[0] == 0);
    check(ok, "trap fired from the zero write");
    ok = (v == 0UL);
    check(ok, "scause readback != 0 after write");
    cks_feed(v);

    // Phase 2: delegate supervisor environment calls. Write
    // exactly bit 9, not boot|bit 9: the boot mideleg (0x1444)
    // already delegates illegal instructions (bit 2), and the
    // phase-complete signal below is a deliberate illegal
    // instruction that must reach M-mode. The readback must have
    // bit 9 set so the delegated state is proven to take effect
    // rather than be silently ignored.
    uart_puts("\nphase 2: delegate, then trap from S-mode\n");
    __asm__ volatile("csrw medeleg, %0"
                     :
                     : "r"(MEDELEG_ECALL_S)
                     : "memory");
    __asm__ volatile("csrr %0, medeleg" : "=r"(rb));
    uart_puts("  medeleg=");
    uart_put_hex(rb);
    uart_puts(" (exactly bit 9: ecall delegated, illegal-insn stays in M-mode)\n");
    ok = ((rb & MEDELEG_ECALL_S) != 0);
    check(ok, "medeleg bit 9 did not take the write");
    cks_feed(rb);

    // The last software write before the trap: all-ones again.
    // Trap entry must overwrite this with the real cause.
    write_scause(SCAUSE_ALL);
    v = read_scause();
    uart_puts("  pre-drop scause=");
    uart_put_hex(v);
    uart_puts(" (last software write before the S-mode trap)\n");
    ok = (scw_regs[0] == 0);
    check(ok, "trap fired from the pre-drop write");
    cks_feed(v);

    // PMP: with no PMP entry programmed, S-mode has no access to
    // any address. Open the whole address space with one NAPOT
    // R/W/X entry before the drop; without this the first S-mode
    // instruction fetch raises an instruction access fault.
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t"  // A=NAPOT, R/W/X, unlocked
                     "csrw pmpcfg0, t0"
                     :
                     :
                     : "t0", "memory");

    uart_puts("dropping to S-mode...\n\n");

    // mret with MPP=01 (S-mode) into scw_smode.
    __asm__ volatile("la t0, scw_smode\n\t"
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
    // returns to M-mode through the trap handler and finishes
    // via the test-device finisher or parks on failure.
    for (;;)
        __asm__ volatile("wfi");
}
