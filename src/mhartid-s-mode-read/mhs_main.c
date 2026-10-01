// mhs_main.c: S-mode read of the M-mode-only mhartid CSR (backlog
// item "riscv mhartid-s-mode-read", 2026-09-30 ideas batch).
//
// Mechanism under test: mhartid is a machine-mode CSR. The
// backlog premise guessed that an S-mode read would succeed and
// agree with the M-mode value; the privileged architecture says
// the opposite, because the CSR number's privilege field reserves
// mhartid for M-mode, so the run measures which behavior the hart
// implements instead of assuming either answer. Exactly one
// mechanism is under test: the privilege gate on one CSR read.
//
// The run, on the QEMU virt board:
//   M-mode boot:
//     1. Read mhartid twice at boot; the two reads must agree and
//        the boot value must be 0 on the virt boot hart.
//     2. Install the M-mode mtvec handler (counts and parks: no
//        M-mode trap is expected), the S-mode stvec handler
//        (records scause/stval/sepc, advances sepc past the
//        faulting instruction, sret), point sscratch at the
//        S-mode save area, set medeleg bit 2 so the
//        illegal-instruction trap is delivered to S-mode, and
//        open a whole-address-space PMP NAPOT entry (S-mode is
//        default-deny without one).
//     3. mret with MPP=01 into mhs_smode_test.
//   S-mode payload:
//     A. Execute csrr mhartid at a labeled 4-byte site with the
//        destination preset to a sentinel. Measured expectation
//        from the architecture: exactly one S-mode trap with
//        scause = 2, sepc exactly at the csrr site, the handler's
//        +4 advance landing on the labeled resume address, and
//        the destination register still holding the sentinel
//        (the read never retired).
//     B. Control: execute csrr sstatus, a CSR S-mode may read.
//        Expected: no new trap and the destination register
//        overwritten, proving the phase A trap is the mhartid
//        privilege gate and not a general CSR failure in S-mode.
//     C. Require that no M-mode trap fired during the run.
//   Verdict: RESULT: PASS only if all 11 checks held. On PASS
//   the virt test-device finisher word shuts the machine down
//   (QEMU exits 0). On FAIL the hart parks in a wfi loop; the
//   bench harness runs QEMU under timeout, so a FAIL is
//   observable as exit status 124.
//
// The fault site uses in-asm numeric local labels (la t, 1f with
// 1: in the asm) so the assembler resolves the exact address;
// &&label addresses are never used for trap-resume addresses.

#include "../uart.h"

extern void mhs_st_trap_entry(void);
extern void mhs_mt_trap_entry(void);

// S-mode trap record, written by mhs_strap.S:
// [0] scause, [1] stval, [2] sepc at entry, [3] trap count,
// [4] sepc after the +4 advance, [5] interrupted t0.
volatile unsigned long mhs_regs[6];

// M-mode trap record, written by mhs_mtrap.S.
volatile unsigned long mhs_mt_traps;

// FNV-1a (64-bit) over the verdict-relevant values, fed in a
// fixed order from both M-mode setup and the S-mode payload, so
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

#define SENTINEL 0xDEADBEEFDEADBEEFUL
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

static unsigned long read_mhartid_m(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

// The S-mode payload. Entered via mret with mstatus.MPP=01.
// Global (not static) because the only reference is the `la` in
// main's inline asm, which the compiler cannot see.
void mhs_smode_test(void) {
    unsigned long site, after, v;
    unsigned long v2;
    unsigned long scause, stval, sepc, sepc_after, traps;
    int ok;

    uart_puts("in S-mode; mhartid read experiment begins\n\n");

    // Phase A: read the M-mode-only hart ID register from
    // S-mode. The architecture reserves this CSR for M-mode, so
    // the read is expected to trap as an illegal instruction
    // rather than return the ID.
    v = SENTINEL;
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "la %1, 2f\n\t"
        "1: csrr %2, mhartid\n\t"
        "2:\n\t"
        ".option pop\n\t"
        : "=r"(site), "=r"(after), "+r"(v)
        :
        : "memory");

    scause = mhs_regs[0];
    stval = mhs_regs[1];
    sepc = mhs_regs[2];
    traps = mhs_regs[3];
    sepc_after = mhs_regs[4];

    uart_puts("phase A: csrr mhartid in S-mode (expect S-mode trap):\n");
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
    uart_puts("\n  csrr dest register = ");
    uart_put_hex(v);
    uart_puts(" (sentinel = ");
    uart_put_hex(SENTINEL);
    uart_puts(")\n");

    ok = (traps == 1);
    check(ok, "phase A: trap count is not 1");
    cks_feed(traps);
    ok = (scause == 2);
    check(ok, "phase A: scause is not 2 (illegal instruction)");
    cks_feed(scause);
    ok = (sepc == site);
    check(ok, "phase A: sepc is not at the csrr site");
    cks_feed(ok);
    ok = (sepc_after == after);
    check(ok, "phase A: sepc+4 is not at the resume label");
    cks_feed(ok);
    ok = (v == SENTINEL);
    check(ok, "phase A: csrr destination changed (the read retired?)");
    cks_feed(ok);
    cks_feed(stval);

    // Phase B: control read of sstatus, a CSR S-mode is allowed
    // to read. It must complete without a new trap and must
    // overwrite its destination, so the phase A trap is charged
    // to the mhartid privilege gate, not to CSR reads in
    // general.
    v2 = SENTINEL;
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "csrr %0, sstatus\n\t"
        ".option pop\n\t"
        : "+r"(v2)
        :
        : "memory");

    traps = mhs_regs[3];
    uart_puts("\nphase B: csrr sstatus in S-mode (expect success):\n");
    uart_puts("  sstatus readback = ");
    uart_put_hex(v2);
    uart_puts("\n  S-mode traps after phase B = ");
    uart_put_dec(traps);
    uart_puts("\n");

    ok = (traps == 1);
    check(ok, "phase B: unexpected S-mode trap on the sstatus read");
    cks_feed(traps);
    ok = (v2 != SENTINEL);
    check(ok, "phase B: sstatus read did not overwrite its destination");
    cks_feed(ok);

    // Phase C: the whole run must have stayed out of M-mode
    // trap handling; delegation delivered the one trap to S.
    ok = (mhs_mt_traps == 0);
    check(ok, "phase C: an M-mode trap fired during the run");
    cks_feed(mhs_mt_traps);

    uart_puts("\nchecksum (FNV-1a over verdict values) = ");
    uart_put_hex64(cksum);
    uart_puts("\n");
    uart_puts("summary: checks=11 mismatches=");
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
    unsigned long boot_id, boot_id2, deleg;
    int ok;

    uart_init();
    uart_puts("\nmhartid-s-mode-read: S-mode read of the M-mode hart ID register\n");

    // 1. Boot baseline in M-mode: two reads must agree, and the
    // boot hart on the virt board reads 0.
    boot_id = read_mhartid_m();
    boot_id2 = read_mhartid_m();
    uart_puts("boot: mhartid=");
    uart_put_hex(boot_id);
    uart_puts(" second=");
    uart_put_hex(boot_id2);
    uart_puts("\n");

    ok = (boot_id == 0);
    check(ok, "boot: mhartid is not 0 on the virt boot hart");
    cks_feed(boot_id);
    ok = (boot_id2 == boot_id);
    check(ok, "boot: second M-mode mhartid read disagrees with the first");
    cks_feed(ok);

    // 2. Trap vectors, delegation, PMP.
    __asm__ volatile("la t0, mhs_mt_trap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mhs_st_trap_entry\n\t"
                     "csrw stvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mhs_regs\n\t"
                     "csrw sscratch, t0"
                     :
                     :
                     : "t0", "memory");
    // Delegate only the illegal-instruction trap (bit 2) to
    // S-mode; every other trap stays in M-mode, where the
    // handler counts and parks.
    __asm__ volatile("li t0, 4\n\t"
                     "csrw medeleg, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("csrr %0, medeleg" : "=r"(deleg));
    uart_puts("medeleg readback=");
    uart_put_hex(deleg);
    uart_puts("\n");
    ok = ((deleg & MEDELEG_ILLEGAL_INSN) != 0);
    check(ok, "medeleg bit 2 (illegal instruction) not set");
    cks_feed(ok);

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

    // 3. mret with MPP=01 (S-mode) into mhs_smode_test. The
    // whole experiment runs in S-mode from there.
    __asm__ volatile("la t0, mhs_smode_test\n\t"
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
