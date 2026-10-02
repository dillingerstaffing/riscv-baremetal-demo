// mhu_main.c: U-mode read of the M-mode-only mhartid CSR (backlog
// item "riscv mhartid-u-mode-read", sibling of the shipped
// src/mhartid-s-mode-read and src/mhartid-readonly modules).
//
// Fundamental truth under test: mhartid is an M-mode-only CSR
// (its number's privilege field reserves it for machine mode),
// so a U-mode read must raise an illegal-instruction trap and
// never deliver the hart ID. Exactly one mechanism is under
// test: the privilege gate on this read from the lowest
// privilege level.
//
// The run, on the QEMU virt board:
//   M-mode boot:
//     1. Save the boot values of medeleg and pmpcfg0 (so the
//        M-mode exit handler can restore them), then read
//        mhartid twice; the two reads must agree and the boot
//        value must be 0 on the virt boot hart.
//     2. Install the M-mode mtvec handler (plans only the
//        ending U-mode ecall; any other M-mode trap counts and
//        parks) and the S-mode stvec handler (records
//        scause/stval/sepc, advances sepc past the faulting
//        instruction, sret), point sscratch at the S-mode save
//        area, set medeleg bit 2 so illegal-instruction traps
//        from U-mode are delivered to S-mode, and open a
//        whole-address-space PMP NAPOT entry (S/U-mode is
//        default-deny without one).
//     3. mret with MPP=00 into mhu_umode_test.
//   U-mode payload:
//     A. Execute csrr mhartid at a labeled 4-byte site with the
//        destination preset to a sentinel. Expect exactly one
//        S-mode trap with scause = 2, sepc exactly at the csrr
//        site, the handler's +4 advance landing on the labeled
//        resume address, and the destination register still
//        holding the sentinel (the read never retired).
//     B. Control: execute csrr sstatus in U-mode, a CSR whose
//        number reserves it for S-mode, also above U-mode.
//        Expect a second S-mode trap, also with scause = 2,
//        sepc at its own site, and the destination still the
//        sentinel: the gate is charged to the privilege level,
//        not to mhartid in particular.
//     C. Require that no M-mode trap fired during the run.
//   End of payload: print the verdict, stash it in mhu_result,
//   and ecall into M-mode, where the handler restores medeleg
//   and pmpcfg0 to their boot values and writes the virt
//   test-device finisher word on PASS (QEMU exits 0) or parks
//   on FAIL (the harness sees a timeout, exit 124).
//
// Fault sites use in-asm numeric local labels (la t, 1f with
// 1: in the asm) so the assembler resolves the exact address;
// &&label addresses are never used for trap-resume addresses.

#include "../uart.h"

extern void mhu_st_trap_entry(void);
extern void mhu_mt_trap_entry(void);

// S-mode trap record, written by mhu_strap.S:
// [0] scause, [1] stval, [2] sepc at entry, [3] trap count,
// [4] sepc after the +4 advance, [5] interrupted t0.
volatile unsigned long mhu_regs[6];

// M-mode trap record, written by mhu_mtrap.S (unexpected
// M-mode traps only; the planned ending ecall does not count).
volatile unsigned long mhu_mt_traps;

// Boot values of the CSRs the run changes; the M-mode exit
// handler restores them before shutdown.
volatile unsigned long mhu_boot_medeleg;
volatile unsigned long mhu_boot_pmpcfg0;

// Verdict stashed for the M-mode exit handler: 1 = PASS,
// 0 = FAIL.
volatile unsigned long mhu_result;

// FNV-1a (64-bit) over the verdict-relevant values, fed in a
// fixed order from M-mode setup and the U-mode payload, so it
// is identical on every passing run.
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

// The U-mode payload. Entered via mret with mstatus.MPP=00.
// Global (not static) because the only reference is the `la` in
// main's inline asm, which the compiler cannot see.
void mhu_umode_test(void) {
    unsigned long site, after, v;
    unsigned long site2, after2, v2;
    unsigned long scause, stval, sepc, sepc_after, traps;
    int ok;

    uart_puts("in U-mode; mhartid read experiment begins\n\n");

    // Phase A: read the M-mode-only hart ID register from
    // U-mode. The CSR number's privilege field reserves it for
    // M-mode, so the read is expected to trap as an illegal
    // instruction rather than return the ID.
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

    scause = mhu_regs[0];
    stval = mhu_regs[1];
    sepc = mhu_regs[2];
    traps = mhu_regs[3];
    sepc_after = mhu_regs[4];

    uart_puts("phase A: csrr mhartid in U-mode (expect S-mode trap):\n");
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

    // Phase B: control read of sstatus, a CSR whose number
    // reserves it for S-mode and above, also above U-mode. It
    // must trap the same way, proving the phase A trap is the
    // privilege-level gate and not something about mhartid in
    // particular.
    v2 = SENTINEL;
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "la %1, 2f\n\t"
        "1: csrr %2, sstatus\n\t"
        "2:\n\t"
        ".option pop\n\t"
        : "=r"(site2), "=r"(after2), "+r"(v2)
        :
        : "memory");

    scause = mhu_regs[0];
    stval = mhu_regs[1];
    sepc = mhu_regs[2];
    traps = mhu_regs[3];
    sepc_after = mhu_regs[4];

    uart_puts("\nphase B: csrr sstatus in U-mode (expect S-mode trap):\n");
    uart_puts("  fault site (label 1)  = ");
    uart_put_hex(site2);
    uart_puts("\n  resume site (label 2) = ");
    uart_put_hex(after2);
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
    uart_put_hex(v2);
    uart_puts(" (sentinel = ");
    uart_put_hex(SENTINEL);
    uart_puts(")\n");

    ok = (traps == 2);
    check(ok, "phase B: trap count is not 2");
    cks_feed(traps);
    ok = (scause == 2);
    check(ok, "phase B: scause is not 2 (illegal instruction)");
    cks_feed(scause);
    ok = (sepc == site2);
    check(ok, "phase B: sepc is not at the csrr site");
    cks_feed(ok);
    ok = (sepc_after == after2);
    check(ok, "phase B: sepc+4 is not at the resume label");
    cks_feed(ok);
    ok = (v2 == SENTINEL);
    check(ok, "phase B: csrr destination changed (the read retired?)");
    cks_feed(ok);
    cks_feed(stval);

    // Phase C: the whole run must have stayed out of M-mode
    // trap handling; delegation delivered the two illegal
    // instructions to S-mode and nothing fell through to M.
    ok = (mhu_mt_traps == 0);
    check(ok, "phase C: an M-mode trap fired during the run");
    cks_feed(mhu_mt_traps);

    uart_puts("\nchecksum (FNV-1a over verdict values) = ");
    uart_put_hex64(cksum);
    uart_puts("\n");
    uart_puts("summary: checks=14 mismatches=");
    uart_put_dec((unsigned long)fails);
    uart_puts("\n");

    uart_puts(fails == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    mhu_result = (fails == 0) ? 1UL : 0UL;

    // End of payload: ecall into M-mode. The M-mode handler
    // restores medeleg and pmpcfg0 to their boot values and,
    // on PASS, writes the virt test-device finisher word. On
    // FAIL it parks, and the harness observes the timeout.
    __asm__ volatile("ecall" ::: "memory");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

int main(void) {
    unsigned long boot_id, boot_id2, deleg;
    int ok;

    uart_init();
    uart_puts("\nmhartid-u-mode-read: U-mode read of the M-mode hart ID register\n");

    // Boot values of the CSRs this run changes; read them
    // before setup so the M-mode exit handler can restore them.
    __asm__ volatile("csrr %0, medeleg" : "=r"(mhu_boot_medeleg));
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(mhu_boot_pmpcfg0));
    uart_puts("boot: medeleg=");
    uart_put_hex(mhu_boot_medeleg);
    uart_puts(" pmpcfg0=");
    uart_put_hex(mhu_boot_pmpcfg0);
    uart_puts("\n");

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
    __asm__ volatile("la t0, mhu_mt_trap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mhu_st_trap_entry\n\t"
                     "csrw stvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mhu_regs\n\t"
                     "csrw sscratch, t0"
                     :
                     :
                     : "t0", "memory");
    // Delegate only the illegal-instruction trap (bit 2) to
    // S-mode; the U-mode ecall at the end of the payload stays
    // in M-mode so the exit handler can restore CSRs. Every
    // other trap stays in M-mode, where the handler counts and
    // parks.
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

    // PMP: with no PMP entry programmed, S/U-mode has no access
    // to any address. Open the whole address space with one
    // NAPOT R/W/X entry before the drop; without this the first
    // U-mode instruction fetch raises an instruction access
    // fault.
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t" // A=NAPOT, R/W/X, unlocked
                     "csrw pmpcfg0, t0\n\t"
                     :
                     :
                     : "t0", "memory");

    uart_puts("setup complete; dropping to U-mode...\n");

    // 3. mret with MPP=00 (U-mode) into mhu_umode_test. The
    // whole experiment runs in U-mode from there.
    __asm__ volatile("la t0, mhu_umode_test\n\t"
                     "csrw mepc, t0\n\t"
                     "csrr t0, mstatus\n\t"
                     "li t1, 0x1800\n\t"   // clear MPP bits 12:11
                     "not t1, t1\n\t"
                     "and t0, t0, t1\n\t"  // MPP = 00 (U-mode)
                     "csrw mstatus, t0\n\t"
                     "mret\n\t"
                     :
                     :
                     : "t0", "t1", "memory");

    // Unreachable: mret lands in the U-mode payload, which ends
    // with an ecall into the M-mode exit handler.
    for (;;)
        __asm__ volatile("wfi");
}
