// mir_main.c: mcounteren IR-bit gate check (backlog item "riscv mcounteren-ir-gate").
//
// Mechanism under test: mcounteren.IR (bit 2) gates S-mode reads of
// instret independently of the CY and TM bits. With mcounteren = 0x3
// (CY and TM set, IR clear) an S-mode rdinstret raises an
// illegal-instruction exception (scause = 2) while an S-mode rdcycle
// at the same privilege level still reads and advances. Reopening the
// gate from M-mode (mcounteren = 0x7) makes two S-mode rdinstret
// reads succeed with strictly increasing samples. M-mode reads are
// never gated, so the experiment needs a real privilege drop to
// observe the gate, and S-mode cannot write mcounteren, so the second
// half needs a real return to M-mode (via ecall) to reopen the gate.
//
// The run, on the QEMU virt board:
//   M-mode boot:
//     1. Record the boot mcounteren value; probe writability by
//        writing 0x7 (CY|TM|IR) and requiring the readback to be
//        exactly 0x7, so the later 0x3 write is proven to take
//        effect rather than be silently ignored.
//     2. Write mcounteren = 0x3 (CY|TM set, IR clear) and require
//        the readback to be 0x3.
//     3. Install the M-mode mtvec handler (parks the hart unless
//        the entry is the expected ecall from S-mode), the S-mode
//        stvec handler (records scause/stval/sepc, advances sepc
//        past the faulting instruction, sret), point sscratch at
//        the S-mode save area, point mscratch at the M-mode save
//        area, set medeleg bit 2 so the illegal-instruction trap
//        is delivered to S-mode (ecall from S-mode stays
//        undelegated so it reaches M-mode), and open a
//        whole-address-space PMP NAPOT entry (S-mode is
//        default-deny without one).
//     4. mret with MPP=01 into mir_smode_phase_a.
//   S-mode phase A:
//     - Execute rdinstret at a labeled 4-byte site with
//       mcounteren.IR = 0. Expected: exactly one S-mode trap with
//       scause = 2, sepc exactly at the rdinstret site, the
//       handler's +4 advance landing on the labeled resume
//       address, and the destination register still holding its
//       pre-fault sentinel (the rdinstret never retired).
//     - Execute one labeled rdcycle with mcounteren.CY = 1.
//       Expected: no new S-mode trap, a strictly positive sample.
//     - Record the ecall site address and issue ecall to return
//       to M-mode.
//   M-mode ecall handler:
//     - Record mcause/mepc and bump the M-mode trap count. Park
//       the hart unless this is the expected ecall from S-mode
//       (mcause = 9).
//     - Write mcounteren = 0x7, record the readback, then mret
//       with MPP=01 into mir_smode_phase_b.
//   S-mode phase B:
//     - Verify the M-mode record: mcause = 9, mepc exactly at the
//       recorded ecall site, exactly one M-mode trap, and the
//       mcounteren readback exactly 0x7.
//     - Execute two labeled rdinstret reads with the IR bit set.
//       Expected: no new S-mode trap, a strictly positive first
//       sample, and a strictly greater second sample (a short
//       spin loop separates the reads so the increase is
//       guaranteed regardless of the counter's time base).
//   Verdict: RESULT: PASS only if all 17 checks held. On PASS the
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

extern void mir_strap_entry(void);
extern void mir_mtrap_entry(void);

// S-mode trap record, written by mir_strap.S:
// [0] scause, [1] stval, [2] sepc at entry, [3] S-mode trap count,
// [4] sepc after the +4 advance, [5] interrupted t0.
volatile unsigned long mir_st_regs[6];

// M-mode trap record, written by mir_mtrap.S:
// [0] mcause, [1] mepc at entry, [2] M-mode trap count,
// [3] mcounteren readback after the 0x7 reopen, [4] interrupted t0.
volatile unsigned long mir_m_regs[5];

// Address of the ecall instruction issued at the end of S-mode
// phase A; recorded by the payload, verified against the M-mode
// handler's mepc record in phase B.
volatile unsigned long mir_ecall_site;

// FNV-1a (64-bit) over the verdict-relevant values, fed in a fixed
// order from both privilege levels, so it is identical on every
// passing run. Absolute cycle/instret sample values are NOT fed
// (they vary run to run); only the check outcomes and
// deterministic values go into the checksum.
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
    uart_putc('0');
    uart_putc('x');
    for (i = 15; i >= 0; i--) {
        unsigned int d = (unsigned int)((v >> (4 * i)) & 0xfULL);
        uart_putc(d < 10 ? (char)('0' + d) : (char)('a' + d - 10));
    }
}

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

#define SENTINEL 0xDEADBEEFDEADBEEFUL
#define MCOUNTEREN_CY (1UL << 0)
#define MCOUNTEREN_TM (1UL << 1)
#define MCOUNTEREN_IR (1UL << 2)
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

static unsigned long read_mcounteren(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mcounteren" : "=r"(v));
    return v;
}

static void write_mcounteren(unsigned long v) {
    __asm__ volatile("csrw mcounteren, %0" ::"r"(v));
}

// S-mode phase A: IR gate closed (mcounteren = 0x3). Entered via
// mret with mstatus.MPP=01. Global (not static) because the only
// references are the `la` instructions in main's inline asm and in
// mir_mtrap.S, which the compiler cannot see.
void mir_smode_phase_a(void) {
    unsigned long site, after, v;
    unsigned long site_c, after_c, c0;
    unsigned long scause, stval, sepc, sepc_after, traps;
    int ok;

    uart_puts("in S-mode; phase A: IR gate closed (mcounteren=0x3)\n\n");

    // Gated read: mcounteren = 0x3 (IR clear), so this S-mode
    // rdinstret must trap.
    v = SENTINEL;
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "la %1, 2f\n\t"
        "1: rdinstret %2\n\t"
        "2:\n\t"
        ".option pop\n\t"
        : "=r"(site), "=r"(after), "+r"(v)
        :
        : "memory");

    scause = mir_st_regs[0];
    stval = mir_st_regs[1];
    sepc = mir_st_regs[2];
    traps = mir_st_regs[3];
    sepc_after = mir_st_regs[4];

    uart_puts("gated: rdinstret with mcounteren=0x3 (expect S-mode trap):\n");
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
    uart_puts("\n  rdinstret dest register = ");
    uart_put_hex(v);
    uart_puts(" (sentinel = ");
    uart_put_hex(SENTINEL);
    uart_puts(")\n");

    ok = (traps == 1);
    check(ok, "phase A: S-mode trap count is not 1");
    cks_feed(traps);
    ok = (scause == 2);
    check(ok, "phase A: scause is not 2 (illegal instruction)");
    cks_feed(scause);
    ok = (sepc == site);
    check(ok, "phase A: sepc is not at the rdinstret site");
    cks_feed(ok);
    ok = (sepc_after == after);
    check(ok, "phase A: sepc+4 is not at the resume label");
    cks_feed(ok);
    ok = (v == SENTINEL);
    check(ok, "phase A: rdinstret destination changed (it retired?)");
    cks_feed(ok);

    // Ungated read: with the CY bit still set, one S-mode rdcycle
    // must succeed with no new trap and a strictly positive
    // sample, proving the CY gate is independent of the IR gate.
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "la %1, 2f\n\t"
        "1: rdcycle %2\n\t"
        "2:\n\t"
        ".option pop\n\t"
        : "=r"(site_c), "=r"(after_c), "=r"(c0)
        :
        : "memory");

    traps = mir_st_regs[3];
    uart_puts("\nungated: one rdcycle with mcounteren=0x3 (expect success):\n");
    uart_puts("  fault site (label 1) = ");
    uart_put_hex(site_c);
    uart_puts("\n  resume site (label 2) = ");
    uart_put_hex(after_c);
    uart_puts("\n  sample = ");
    uart_put_hex(c0);
    uart_puts("\n  S-mode traps after rdcycle = ");
    uart_put_dec(traps);
    uart_puts("\n");

    ok = (traps == 1);
    check(ok, "phase A: unexpected S-mode trap on rdcycle");
    cks_feed(traps);
    ok = (c0 > 0);
    check(ok, "phase A: rdcycle sample is not strictly positive");
    cks_feed(ok);

    // Return to M-mode to reopen the IR gate. medeleg leaves the
    // S-mode ecall undelegated, so it traps to M-mode. The ecall
    // site address is stored to mir_ecall_site BEFORE the ecall
    // executes: the trap means no instruction after the ecall
    // ever runs in phase A, so a C store placed after the asm
    // would never execute.
    uart_puts("\nphase A done; ecall to M-mode to reopen the IR gate\n");
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la t0, 1f\n\t"
        "la t1, mir_ecall_site\n\t"
        "sd t0, 0(t1)\n\t"
        "1: ecall\n\t"
        ".option pop\n\t"
        :
        :
        : "t0", "t1", "memory");

    // Unreachable: the M-mode ecall handler mrets into phase B,
    // never back here.
    for (;;)
        __asm__ volatile("wfi");
}

// S-mode phase B: IR gate reopened (mcounteren = 0x7). Entered via
// mret from the M-mode ecall handler with mstatus.MPP=01.
void mir_smode_phase_b(void) {
    unsigned long site, after, i0, i1;
    unsigned long mcause, mmepc, mtraps, reopened, traps;
    int ok;

    uart_puts("\nin S-mode; phase B: IR gate reopened (mcounteren=0x7)\n\n");

    mcause = mir_m_regs[0];
    mmepc = mir_m_regs[1];
    mtraps = mir_m_regs[2];
    reopened = mir_m_regs[3];

    uart_puts("M-mode ecall record:\n");
    uart_puts("  mcause = ");
    uart_put_hex(mcause);
    uart_puts("\n  mepc = ");
    uart_put_hex(mmepc);
    uart_puts("\n  ecall site (phase A) = ");
    uart_put_hex(mir_ecall_site);
    uart_puts("\n  M-mode traps = ");
    uart_put_dec(mtraps);
    uart_puts("\n  mcounteren readback after reopen = ");
    uart_put_hex(reopened);
    uart_puts("\n");

    ok = (mcause == 9);
    check(ok, "phase B: mcause is not 9 (ecall from S-mode)");
    cks_feed(ok);
    ok = (mmepc == mir_ecall_site);
    check(ok, "phase B: mepc is not at the ecall site");
    cks_feed(ok);
    ok = (mtraps == 1);
    check(ok, "phase B: M-mode trap count is not 1");
    cks_feed(mtraps);
    ok = (reopened == 0x7UL);
    check(ok, "phase B: mcounteren reopen readback is not 0x7");
    cks_feed(reopened);

    // Enabled reads: with the IR bit set, two S-mode rdinstret
    // reads must succeed with no new trap, a strictly positive
    // first sample, and a strictly greater second sample. The
    // spin loop between the reads guarantees the increase no
    // matter what time base the counter is derived from.
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "la %1, 2f\n\t"
        "1: rdinstret %2\n\t"
        "2:\n\t"
        ".option pop\n\t"
        : "=r"(site), "=r"(after), "=r"(i0)
        :
        : "memory");
    __asm__ volatile(
        "li t0, 200000\n\t"
        "1: addi t0, t0, -1\n\t"
        "bnez t0, 1b\n\t"
        :
        :
        : "t0", "memory");
    __asm__ volatile("rdinstret %0" : "=r"(i1));

    traps = mir_st_regs[3];
    uart_puts("\nenabled: two rdinstret with mcounteren=0x7 (expect success):\n");
    uart_puts("  fault site (label 1) = ");
    uart_put_hex(site);
    uart_puts("\n  resume site (label 2) = ");
    uart_put_hex(after);
    uart_puts("\n  sample 1 = ");
    uart_put_hex(i0);
    uart_puts("\n  sample 2 = ");
    uart_put_hex(i1);
    uart_puts("  (delta = ");
    uart_put_dec(i1 - i0);
    uart_puts(")\n");
    uart_puts("  S-mode traps after phase B = ");
    uart_put_dec(traps);
    uart_puts("\n");

    ok = (traps == 1);
    check(ok, "phase B: unexpected S-mode trap on rdinstret");
    cks_feed(traps);
    ok = (i0 > 0);
    check(ok, "phase B: first rdinstret sample is not strictly positive");
    cks_feed(ok);
    ok = (i1 > i0);
    check(ok, "phase B: rdinstret samples did not advance");
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
    uart_puts("mcounteren: S-mode rdinstret gating trap\n");
    uart_puts("mcounteren=0x3: rdinstret must trap (IR clear);\n");
    uart_puts("reopened to 0x7: rdinstret must read and advance\n");
    uart_puts("========================================\n\n");

    // 1. Boot mcounteren and the writability probe: write 0x7
    // (CY|TM|IR), read back; the readback must be exactly 0x7 so
    // the later 0x3 write is proven to take effect rather than be
    // silently ignored (a lone 0x3 write would be ambiguous if
    // the CSR were read-only-zero).
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

    // 2. Gate: write mcounteren = 0x3 (CY|TM set, IR clear), read
    // back.
    write_mcounteren(MCOUNTEREN_CY | MCOUNTEREN_TM);
    rb = read_mcounteren();
    uart_puts("write: csrw mcounteren, 0x3; readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == 0x3UL, "mcounteren write of 0x3 did not read back as 0x3");
    cks_feed(rb);

    // 3. Trap vectors, delegation, PMP.
    __asm__ volatile("la t0, mir_mtrap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mir_strap_entry\n\t"
                     "csrw stvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mir_st_regs\n\t"
                     "csrw sscratch, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mir_m_regs\n\t"
                     "csrw mscratch, t0"
                     :
                     :
                     : "t0", "memory");
    // Delegate only the illegal-instruction trap (bit 2) to
    // S-mode. The ecall from S-mode (bit 8) stays undelegated so
    // it reaches M-mode, where the handler reopens the IR gate.
    // A correct run takes no other M-mode trap at all.
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

    uart_puts("setup complete; dropping to S-mode (phase A)...\n");

    // 4. mret with MPP=01 (S-mode) into mir_smode_phase_a. The
    // whole experiment runs in S-mode from there.
    __asm__ volatile("la t0, mir_smode_phase_a\n\t"
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

    // Unreachable: mret lands in the S-mode phase-A payload, which
    // finishes via the M-mode ecall handler and the phase-B
    // payload, or parks on failure.
    for (;;)
        __asm__ volatile("wfi");
}
