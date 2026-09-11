// tw_main.c: mstatus.TW trap check (backlog item: riscv mstatus-tw-trap).
//
// Mechanism under test: the TW bit (mstatus bit 21) makes WFI
// trap when executed in S-mode. With TW set, an S-mode WFI
// raises an illegal-instruction exception (scause = 2) instead of
// putting the hart to sleep. Observing the gate needs a real
// privilege drop: M-mode WFI is never gated by TW, so the
// experiment sets TW in M-mode and drops to S-mode to run the
// WFI.
//
// The run, on the QEMU virt board:
//   M-mode boot:
//     1. Record the boot mstatus value; set mstatus.TW and require
//        the readback to have bit 21 set, so the later S-mode WFI
//        is proven to run under TW=1 rather than under a silently
//        ignored write.
//     2. Install the M-mode mtvec handler (parks the hart; no
//        M-mode trap is expected on a correct run), the S-mode
//        stvec handler (records scause/stval/sepc, advances sepc
//        past the faulting instruction, sret), point sscratch at
//        the S-mode save area, set medeleg bit 2 so the
//        illegal-instruction trap is delivered to S-mode, and open
//        a whole-address-space PMP NAPOT entry (S-mode is
//        default-deny without one).
//     3. mret with MPP=01 into tw_smode_test.
//   S-mode payload:
//     Execute WFI at a labeled 4-byte site with mstatus.TW = 1.
//     Expected: exactly one S-mode trap with scause = 2, sepc
//     exactly at the WFI site, and the handler's +4 advance
//     landing on the labeled resume address. No second trap is
//     possible: the payload executes no further WFI after the
//     resume.
//   Verdict: RESULT: PASS only if all 6 checks held. On PASS the
//   virt test-device finisher word shuts the machine down (QEMU
//   exits 0). On FAIL the hart parks in a wfi loop; with TW still
//   set each WFI raises the delegated illegal-instruction trap and
//   the handler resumes past it, so the hart spins in a trap loop
//   until the bench harness timeout (exit status 124).
//
// Scope note: the control case (TW clear, WFI in S-mode) is
// intentionally omitted, because an untrapped S-mode WFI would
// block indefinitely with no interrupt source armed and could not
// complete a run. The shipped verification is the TW=1 trap
// behavior together with the mstatus.TW readback, exactly what
// was measured.
//
// The fault site uses in-asm numeric local labels (la t, 1f with
// 1: in the asm) so the assembler resolves the exact address;
// the toolchain miscompiles C &&label at -O2, so &&label is never
// used for trap-resume addresses.

#include "../uart.h"

extern void tw_strap_entry(void);
extern void tw_mtrap_entry(void);

// S-mode trap record, written by tw_strap.S:
// [0] scause, [1] stval, [2] sepc at entry, [3] trap count,
// [4] sepc after the +4 advance, [5] interrupted t0.
volatile unsigned long tw_st_regs[6];

// FNV-1a (64-bit) over the verdict-relevant values, fed in a fixed
// order from both M-mode setup and the S-mode payload, so it is
// identical on every passing run.
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

#define MSTATUS_TW (1UL << 21)
#define MEDELEG_ILLEGAL_INSN (1UL << 2)

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long read_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static void write_mstatus(unsigned long v) {
    __asm__ volatile("csrw mstatus, %0" ::"r"(v));
}

// The S-mode payload. Entered via mret with mstatus.MPP=01.
// Global (not static) because the only reference is the `la` in
// main's inline asm, which the compiler cannot see.
void tw_smode_test(void) {
    unsigned long site, after;
    unsigned long scause, stval, sepc, sepc_after, traps;
    int ok;

    uart_puts("in S-mode; mstatus.TW experiment begins\n\n");

    // With mstatus.TW = 1 this S-mode WFI must raise an
    // illegal-instruction trap instead of sleeping.
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "la %1, 2f\n\t"
        "1: wfi\n\t"
        "2:\n\t"
        ".option pop\n\t"
        : "=r"(site), "=r"(after)
        :
        : "memory");

    scause = tw_st_regs[0];
    stval = tw_st_regs[1];
    sepc = tw_st_regs[2];
    traps = tw_st_regs[3];
    sepc_after = tw_st_regs[4];

    uart_puts("S-mode wfi with mstatus.TW=1 (expect S-mode trap):\n");
    uart_puts("  fault site (label 1)  = ");
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
    uart_put_hex(sepc_after);
    uart_puts("\n  traps   = ");
    uart_put_dec(traps);
    uart_puts("\n");

    ok = (traps == 1);
    check(ok, "trap count is not 1");
    cks_feed(traps);
    ok = (scause == 2);
    check(ok, "scause is not 2 (illegal instruction)");
    cks_feed(scause);
    ok = (sepc == site);
    check(ok, "sepc is not at the wfi site");
    cks_feed(ok);
    ok = (sepc_after == after);
    check(ok, "sepc+4 is not at the resume label");
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

int main(void) {
    unsigned long boot_ms, rb, deleg;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("mstatus.TW: S-mode wfi must trap\n");
    uart_puts("TW=1: S-mode wfi raises illegal instruction\n");
    uart_puts("========================================\n\n");

    // 1. Boot mstatus and the TW write: OR bit 21 in, read back;
    // the readback must have the bit set so the later S-mode WFI
    // is proven to run under TW=1 rather than under a silently
    // ignored write.
    boot_ms = read_mstatus();
    uart_puts("boot: mstatus=");
    uart_put_hex(boot_ms);
    uart_puts("\n");

    write_mstatus(boot_ms | MSTATUS_TW);
    rb = read_mstatus();
    uart_puts("write: mstatus|TW; readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check((rb & MSTATUS_TW) != 0, "mstatus.TW did not read back as set");
    cks_feed((rb & MSTATUS_TW) != 0);

    // 2. Trap vectors, delegation, PMP.
    __asm__ volatile("la t0, tw_mtrap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, tw_strap_entry\n\t"
                     "csrw stvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, tw_st_regs\n\t"
                     "csrw sscratch, t0"
                     :
                     :
                     : "t0", "memory");
    // Delegate only the illegal-instruction trap (bit 2) to
    // S-mode; a correct run takes no M-mode trap at all.
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

    // 3. mret with MPP=01 (S-mode) into tw_smode_test. The whole
    // experiment runs in S-mode from there. The 0x1800 mask
    // clears only the MPP bits, so TW (bit 21) stays set across
    // the drop.
    __asm__ volatile("la t0, tw_smode_test\n\t"
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
