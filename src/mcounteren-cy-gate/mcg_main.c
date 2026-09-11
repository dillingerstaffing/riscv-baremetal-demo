// mcg_main.c: mcounteren cycle-counter gate check (backlog item
// "riscv mcounteren-cycle-gate").
//
// Mechanism under test: mcounteren.CY (bit 0) gates S-mode reads
// of the cycle counter. With mcounteren.CY = 0 an S-mode rdcycle
// must raise an illegal-instruction exception (scause = 2) while
// with mcounteren.CY = 1 the same read must succeed and the
// counter must advance. This is the cycle-counter counterpart of
// src/mcounteren-ir-gate (instret) and src/mcounteren-time-gate
// (time); the three experiments differ only in which counter bit
// is cleared and gated. M-mode reads are never gated, so the
// experiment needs a real privilege drop to observe the gate.
//
// The run, on the QEMU virt board:
//   M-mode boot:
//     1. Record the boot mcounteren value (QEMU resets it to 0);
//        probe writability by writing 0x7 (CY|TM|IR) and requiring
//        the readback to be exactly 0x7, so the later gate writes
//        are proven to take effect rather than be silently
//        ignored.
//     2. Write mcounteren = 0x0 (CY clear) and require the
//        readback to be 0x0.
//     3. Install the M-mode mtvec handler (handles exactly one
//        expected S-mode ecall between the phases: it sets
//        mcounteren = 0x1, records the readback, advances mepc
//        past the ecall, and mret back to S-mode; any other
//        M-mode trap parks the hart), the S-mode stvec handler
//        (records scause/stval/sepc, advances sepc past the
//        faulting instruction, sret), point mscratch at the
//        M-mode record and sscratch at the S-mode save area, set
//        medeleg bit 2 so the illegal-instruction trap is
//        delivered to S-mode, and open a whole-address-space PMP
//        NAPOT entry (S-mode is default-deny without one).
//     4. mret with MPP=01 into mcg_smode_test.
//   S-mode payload:
//     A. Execute rdcycle at a labeled 4-byte site with
//        mcounteren.CY = 0. Expected: exactly one S-mode trap with
//        scause = 2, sepc exactly at the rdcycle site, the
//        handler's +4 advance landing on the labeled resume
//        address, and the destination register still holding its
//        pre-fault sentinel (the rdcycle never retired).
//     B. Issue an ecall back to M-mode; the handler sets
//        mcounteren = 0x1 (CY set, TM and IR clear) and resumes in
//        S-mode. Expected: exactly one M-mode trap with
//        mcause = 9, mepc exactly at the ecall site, and an
//        mcounteren readback of 0x1.
//     C. Execute rdcycle twice with mcounteren.CY = 1. Expected:
//        no new S-mode trap, a strictly positive first sample,
//        and a strictly increasing second sample.
//   Verdict: RESULT: PASS only if all 15 checks held. On PASS the
//   virt test-device finisher word shuts the machine down (QEMU
//   exits 0). On FAIL the hart parks in a wfi loop; the bench
//   harness runs QEMU under timeout, so a FAIL is observable as
//   exit status 124.
//
// The fault sites use in-asm numeric local labels (la t, 1f with
// 1: in the asm) so the assembler resolves the exact address;
// the toolchain miscompiles C &&label at -O2, so &&label is never
// used for trap-resume addresses.

#include "../uart.h"

extern void mcg_strap_entry(void);
extern void mcg_mtrap_entry(void);

// S-mode trap record, written by mcg_strap.S:
// [0] scause, [1] stval, [2] sepc at entry, [3] trap count,
// [4] sepc after the +4 advance, [5] interrupted t0.
volatile unsigned long mcg_st_regs[6];

// M-mode trap record, written by mcg_mtrap.S:
// [0] mcause, [1] mepc at entry, [2] M-mode trap count,
// [3] mcounteren readback after the handler's set, [4] saved t1,
// [5] saved t2.
volatile unsigned long mcg_m_regs[6];

// FNV-1a (64-bit) over the verdict-relevant values, fed in a fixed
// order from both privilege levels, so it is identical on every
// passing run. Absolute cycle-counter samples are NOT fed (they
// vary run to run); only the check outcomes and deterministic
// values go into the checksum.
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
#define MCOUNTEREN_CY (1UL << 0)
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

static unsigned long read_mcounteren(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mcounteren" : "=r"(v));
    return v;
}

static void write_mcounteren(unsigned long v) {
    __asm__ volatile("csrw mcounteren, %0" ::"r"(v));
}

// The S-mode payload. Entered via mret with mstatus.MPP=01.
// Global (not static) because the only reference is the `la` in
// main's inline asm, which the compiler cannot see.
void mcg_smode_test(void) {
    unsigned long site, after, v;
    unsigned long ecall_site;
    unsigned long mcause, mepc, mtraps, mc_rb;
    unsigned long site2, after2, c0, c1;
    unsigned long scause, stval, sepc, sepc_after, traps;
    int ok;

    uart_puts("in S-mode; mcounteren cycle-gate experiment begins\n\n");

    // Phase A: mcounteren = 0x0 (CY clear), so this S-mode
    // rdcycle must trap.
    v = SENTINEL;
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "la %1, 2f\n\t"
        "1: rdcycle %2\n\t"
        "2:\n\t"
        ".option pop\n\t"
        : "=r"(site), "=r"(after), "+r"(v)
        :
        : "memory");

    scause = mcg_st_regs[0];
    stval = mcg_st_regs[1];
    sepc = mcg_st_regs[2];
    traps = mcg_st_regs[3];
    sepc_after = mcg_st_regs[4];

    uart_puts("phase A: rdcycle with mcounteren=0x0 (expect S-mode trap):\n");
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
    uart_puts("\n  rdcycle dest register = ");
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
    check(ok, "phase A: sepc is not at the rdcycle site");
    cks_feed(ok);
    ok = (sepc_after == after);
    check(ok, "phase A: sepc+4 is not at the resume label");
    cks_feed(ok);
    ok = (v == SENTINEL);
    check(ok, "phase A: rdcycle destination changed (it retired?)");
    cks_feed(ok);

    // Phase B: ecall back to M-mode so the handler can set
    // mcounteren.CY. QEMU resets mcounteren to 0, so the set must
    // be explicit, in M-mode, and its readback recorded.
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "1: ecall\n\t"
        ".option pop\n\t"
        : "=r"(ecall_site)
        :
        : "memory");

    mcause = mcg_m_regs[0];
    mepc = mcg_m_regs[1];
    mtraps = mcg_m_regs[2];
    mc_rb = mcg_m_regs[3];

    uart_puts("\nphase B: ecall to M-mode to set mcounteren=0x1:\n");
    uart_puts("  ecall site (label 1) = ");
    uart_put_hex(ecall_site);
    uart_puts("\n  mcause  = ");
    uart_put_hex(mcause);
    uart_puts("\n  mepc    = ");
    uart_put_hex(mepc);
    uart_puts("\n  M-mode traps = ");
    uart_put_dec(mtraps);
    uart_puts("\n  mcounteren readback after set = ");
    uart_put_hex(mc_rb);
    uart_puts("\n");

    ok = (mtraps == 1);
    check(ok, "phase B: M-mode trap count is not 1");
    cks_feed(mtraps);
    ok = (mcause == MCAUSE_ECALL_SMODE);
    check(ok, "phase B: mcause is not 9 (ecall from S-mode)");
    cks_feed(mcause);
    ok = (mepc == ecall_site);
    check(ok, "phase B: mepc is not at the ecall site");
    cks_feed(ok);
    ok = (mc_rb == MCOUNTEREN_CY);
    check(ok, "phase B: mcounteren readback is not 0x1 after set");
    cks_feed(mc_rb);

    // Phase C: with CY set, the S-mode rdcycle reads must
    // succeed: no new trap, a strictly positive first sample, and
    // a strictly increasing second sample.
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "la %1, 2f\n\t"
        "1: rdcycle %2\n\t"
        "2:\n\t"
        ".option pop\n\t"
        : "=r"(site2), "=r"(after2), "=r"(c0)
        :
        : "memory");
    __asm__ volatile("rdcycle %0" : "=r"(c1));

    traps = mcg_st_regs[3];
    uart_puts("\nphase C: rdcycle with mcounteren=0x1 (expect success):\n");
    uart_puts("  fault site (label 1) = ");
    uart_put_hex(site2);
    uart_puts("\n  sample 1 = ");
    uart_put_hex(c0);
    uart_puts("\n  sample 2 = ");
    uart_put_hex(c1);
    uart_puts("  (delta = ");
    uart_put_dec(c1 - c0);
    uart_puts(")\n");
    uart_puts("  S-mode traps after phase C = ");
    uart_put_dec(traps);
    uart_puts("\n");

    ok = (traps == 1);
    check(ok, "phase C: unexpected S-mode trap on rdcycle");
    cks_feed(traps);
    ok = (c0 > 0);
    check(ok, "phase C: first rdcycle sample is not strictly positive");
    cks_feed(ok);
    ok = (c1 > c0);
    check(ok, "phase C: rdcycle samples did not advance");
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
    unsigned long boot_mc, rb, deleg;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("mcounteren: S-mode rdcycle gating trap\n");
    uart_puts("mcounteren=0x0: rdcycle must trap; 0x1: it must read\n");
    uart_puts("========================================\n\n");

    // 1. Boot mcounteren and the writability probe: write 0x7
    // (CY|TM|IR), read back; the readback must be exactly 0x7 so
    // the later gate writes are proven to take effect rather than
    // be silently ignored (a lone 0x0 gate write would be
    // ambiguous if the CSR were read-only-zero).
    boot_mc = read_mcounteren();
    uart_puts("boot: mcounteren=");
    uart_put_hex(boot_mc);
    uart_puts("\n");

    write_mcounteren(0x7UL);
    rb = read_mcounteren();
    uart_puts("probe: csrw mcounteren, 0x7; readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == 0x7UL, "mcounteren write of 0x7 did not read back as 0x7");
    cks_feed(rb);

    // 2. Gate: write mcounteren = 0x0 (CY clear), read back.
    write_mcounteren(0x0UL);
    rb = read_mcounteren();
    uart_puts("write: csrw mcounteren, 0x0; readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == 0x0UL, "mcounteren write of 0x0 did not read back as 0x0");
    cks_feed(rb);

    // 3. Trap vectors, delegation, PMP.
    __asm__ volatile("la t0, mcg_mtrap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mcg_strap_entry\n\t"
                     "csrw stvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mcg_m_regs\n\t"
                     "csrw mscratch, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mcg_st_regs\n\t"
                     "csrw sscratch, t0"
                     :
                     :
                     : "t0", "memory");
    // Delegate only the illegal-instruction trap (bit 2) to
    // S-mode; the S-mode ecall between the phases stays in
    // M-mode so the handler can set mcounteren.CY there. A
    // correct run takes no other M-mode trap.
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

    // 4. mret with MPP=01 (S-mode) into mcg_smode_test. The whole
    // experiment runs in S-mode from there, with one M-mode
    // excursion via ecall between the phases.
    __asm__ volatile("la t0, mcg_smode_test\n\t"
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
