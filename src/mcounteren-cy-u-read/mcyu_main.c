// mcyu_main.c: mcounteren.CY gating of U-mode rdcycle (backlog
// item mcounteren-cy-u-read).
//
// Mechanism under test: mcounteren.CY is the M-level gate for the
// cycle CSR. With scounteren.CY held set, clearing mcounteren.CY
// must turn a U-mode rdcycle into an illegal-instruction trap
// (scause = 2, destination register untouched); with CY set, the
// same U-mode rdcycle must succeed and return an advancing
// count. This is the converse of the done scounteren-cy-gate
// module, which held mcounteren.CY set and toggled the S-level
// gate; here the S-level gate is held set and the M-level gate
// is toggled.
//
// One subtlety the setup must get right: a U-mode rdcycle is
// gated by BOTH mcounteren.CY (M-level) and scounteren.CY
// (S-level), and QEMU resets both to 0. If scounteren.CY stayed
// clear, phase A would trap regardless of mcounteren.CY and
// prove nothing. M-mode therefore sets scounteren.CY during
// setup and it is never cleared afterwards, so mcounteren.CY is
// the only gate under test.
//
// The run, on the QEMU virt board:
//   M-mode boot:
//     1. Record the boot mcounteren value (QEMU resets it to 0);
//        probe writability by writing 0x7 (CY|TM|IR) and
//        requiring the readback to be exactly 0x7, so the later
//        gate writes are proven to take effect rather than be
//        silently ignored.
//     2. Write mcounteren = 0x1 (CY set) and require the
//        readback to carry CY.
//     3. Write scounteren = 0x1 (CY set) and require the
//        readback to carry CY, so the S-level gate never masks
//        the M-level gate under test.
//     4. Install the M-mode trap entry (handles exactly one
//        expected S-mode ecall between the phases: it verifies
//        mcause/mepc, requires scounteren to still read 0x1,
//        clears mcounteren.CY, requires the 0 readback, and
//        mrets with MPP=01 into the phase-B S-mode driver; any
//        other M-mode trap takes the FAIL path), the S-mode
//        stvec handler (records scause/stval/sepc and the
//        interrupted t0/a0/t1/s0, appends each trap to a
//        history, and resumes where the C dispatcher decides),
//        point sscratch at the S-mode save area, set medeleg
//        bits 2 and 8 so the illegal-instruction trap and the
//        U-mode ecall are delivered to S-mode (S-mode ecalls
//        stay in M-mode), and open a whole-address-space PMP
//        NAPOT entry (U-mode is default-deny without one).
//     5. mret with MPP=01 into mcyu_smode_test.
//   S-mode driver:
//     A. sret with SPP=0 into the U-mode payload A with
//        mcounteren.CY=1. Expected: no trap from either
//        rdcycle; the payload's ecall (cause 8, signal SIG_A)
//        carries the two samples in the interrupted t0/t1. The
//        dispatcher answers by resuming in S-mode at
//        mcyu_phase_a_done, which requires strictly increasing
//        samples and a nonzero first sample, then issues an
//        S-mode ecall (cause 9, not delegated) to hand back to
//        M-mode.
//     B. The M-mode handler clears mcounteren.CY, requires the
//        0 readback, requires scounteren to still read 0x1,
//        and mrets with MPP=01 into mcyu_phase_b_start, which
//        srets with SPP=0 into the U-mode payload B. Expected:
//        exactly two more S-mode traps. Trap 2 is the gated
//        rdcycle: scause = 2, sepc exactly at the rdcycle site,
//        and the interrupted t0 still holding its pre-fault
//        sentinel (the rdcycle never retired). Trap 3 is the
//        payload's ecall (cause 8, signal SIG_B), which the
//        dispatcher answers by resuming in S-mode at
//        mcyu_phase_b_done.
//   Verdict: RESULT: PASS only if all 22 checks held. On PASS
//   the virt test-device finisher word shuts the machine down
//   (QEMU exits 0). On FAIL the hart parks in a wfi loop; the
//   bench harness runs QEMU under timeout, so a FAIL is
//   observable as exit status 124.
//
// The fault and signal sites use assembler-resolved global
// labels in mcyu_trap.S (never C &&label: the toolchain
// miscompiles labels-as-values at -O2, so &&label is never used
// for trap-resume addresses).

#include "../uart.h"

extern void mcyu_trap_entry(void);
extern void mcyu_mt_trap_entry(void);
extern void mcyu_s_ecall(void);

// U-mode payload sites, defined in mcyu_trap.S.
extern char mcyu_u_payload_a[];
extern char mcyu_u_rdcycle_a[];
extern char mcyu_u_ecall_a[];
extern char mcyu_u_payload_b[];
extern char mcyu_u_rdcycle_b[];
extern char mcyu_u_ecall_b[];
extern char mcyu_s_ecall_site[];

// S-mode continuations, entered via sret from the dispatcher or
// via mret from the M-mode handler.
void mcyu_smode_test(void);
void mcyu_phase_a_done(void);
void mcyu_phase_b_start(void);
void mcyu_phase_b_done(void);
void mcyu_unexpected(void);

// M-mode handoff handler, called from mcyu_trap.S on the
// expected S-mode ecall. Arms phase B and mrets; never returns.
void mcyu_m_handle(void);

// S-mode trap record, written by mcyu_trap.S:
// [0] scause, [1] stval, [2] sepc at entry, [3] trap count,
// [4] resume pc chosen by the dispatcher, [5] interrupted t0,
// [6] interrupted a0, [7] interrupted t1, [8] interrupted s0.
volatile unsigned long mcyu_regs[10];

// M-mode trap record, written by mcyu_trap.S: [0] mcause,
// [1] mepc at entry.
volatile unsigned long mcyu_m_regs[2];

// Trap history: the dispatcher copies every S-mode trap's
// record here so phase B can still inspect the gated rdcycle
// trap after the closing ecall trap overwrote mcyu_regs.
#define MCYU_HIST_N 8
volatile unsigned long mcyu_hist[MCYU_HIST_N][10];
volatile unsigned long mcyu_hist_n;
volatile unsigned long mcyu_bad_cause;
volatile unsigned long mcyu_bad_sig;

// Site addresses, captured in M-mode before the first drop.
volatile unsigned long mcyu_site_rdcycle_a;
volatile unsigned long mcyu_site_ecall_a;
volatile unsigned long mcyu_site_rdcycle_b;
volatile unsigned long mcyu_site_ecall_b;

#define MCYU_SIG_A 0x9a9aUL
#define MCYU_SIG_B 0x9b9bUL

#define SENTINEL 0xDEADBEEFDEADBEEFUL
#define MCOUNTEREN_CY (1UL << 0)
#define SCOUNTEREN_CY (1UL << 0)
#define MEDELEG_ILLEGAL_INSN (1UL << 2)
#define MEDELEG_ECALL_U (1UL << 8)

// FNV-1a (64-bit) over the verdict-relevant values, fed in a
// fixed order from every privilege level. Only run-invariant
// values are fed (causes, counts, check booleans), never raw
// cycle samples, so the checksum is identical on every passing
// run.
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
// the final lines from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

static void uart_drain(void) {
    while ((*UART0_LSR & LSR_TEMT) == 0)
        ;
}

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// sret into a U-mode payload: sepc at the payload, sstatus.SPP
// cleared to U-mode. Never returns; the payload's traps come
// back through the S-mode handler and the dispatcher resumes at
// an S-mode continuation.
static void drop_to_umode(void (*payload)(void)) {
    __asm__ volatile("csrw sepc, %0\n\t"
                     "li t0, 0x100\n\t"       // sstatus.SPP is bit 8
                     "csrrc t0, sstatus, t0\n\t" // SPP = 0 (U-mode)
                     "sret\n\t"
                     ::"r"(payload)
                     : "t0", "memory");
    for (;;)
        __asm__ volatile("wfi");
}

// C dispatcher, called from mcyu_trap.S with r = mcyu_regs.
// Appends the trap record to the history and returns the resume
// pc: sepc+4 for the gated rdcycle (illegal instruction), or an
// S-mode continuation for the U-mode ecall signals (setting
// sstatus.SPP=1 first so sret resumes in S-mode).
unsigned long mcyu_handle(volatile unsigned long *r) {
    unsigned long cause = r[0];
    unsigned long resume;
    int i;

    if (mcyu_hist_n < MCYU_HIST_N) {
        for (i = 0; i < 9; i++)
            mcyu_hist[mcyu_hist_n][i] = r[i];
        mcyu_hist_n++;
    }

    if (cause == 2) {
        resume = r[2] + 4;
        r[4] = resume;
        return resume;
    }
    if (cause == 8) {
        unsigned long sig = r[6];
        __asm__ volatile("li t0, 0x100\n\t"
                         "csrs sstatus, t0"  // SPP = 1: sret resumes in S-mode
                         ::: "t0", "memory");
        if (sig == MCYU_SIG_A)
            resume = (unsigned long)mcyu_phase_a_done;
        else if (sig == MCYU_SIG_B)
            resume = (unsigned long)mcyu_phase_b_done;
        else {
            mcyu_bad_cause = cause;
            mcyu_bad_sig = sig;
            resume = (unsigned long)mcyu_unexpected;
        }
        r[4] = resume;
        return resume;
    }
    mcyu_bad_cause = cause;
    mcyu_bad_sig = r[6];
    __asm__ volatile("li t0, 0x100\n\t"
                     "csrs sstatus, t0"
                     ::: "t0", "memory");
    resume = (unsigned long)mcyu_unexpected;
    r[4] = resume;
    return resume;
}

static void print_trap(int idx, const char *tag) {
    volatile unsigned long *h = mcyu_hist[idx];
    uart_puts("  [");
    uart_put_dec((unsigned long)idx);
    uart_puts("] ");
    uart_puts(tag);
    uart_puts(" scause=");
    uart_put_hex(h[0]);
    uart_puts(" stval=");
    uart_put_hex(h[1]);
    uart_puts(" sepc=");
    uart_put_hex(h[2]);
    uart_puts(" t0=");
    uart_put_hex(h[5]);
    uart_puts(" a0=");
    uart_put_hex(h[6]);
    uart_puts("\n");
}

// Phase A continuation, entered in S-mode via sret from the
// dispatcher after the payload's SIG_A ecall.
void mcyu_phase_a_done(void) {
    unsigned long s0, s1;
    int ok;

    uart_puts("\n--- back in S-mode: phase A complete ---\n");
    uart_puts("trap history:\n");
    print_trap(0, "SIG_A ecall ");

    ok = (mcyu_hist_n == 1);
    check(ok, "phase A: trap count is not 1");
    cks_feed(ok);
    ok = (mcyu_hist[0][0] == 8);
    check(ok, "phase A: trap 1 scause is not 8 (ecall from U-mode)");
    cks_feed(mcyu_hist[0][0]);
    ok = (mcyu_hist[0][2] == mcyu_site_ecall_a);
    check(ok, "phase A: trap 1 sepc is not at the payload ecall site");
    cks_feed(ok);
    ok = (mcyu_hist[0][6] == MCYU_SIG_A);
    check(ok, "phase A: trap 1 signal is not SIG_A");
    cks_feed(ok);

    s0 = mcyu_hist[0][5];
    s1 = mcyu_hist[0][7];
    uart_puts("  rdcycle sample 0 (t0) = ");
    uart_put_hex(s0);
    uart_puts("\n  rdcycle sample 1 (t1) = ");
    uart_put_hex(s1);
    uart_puts("  (delta = ");
    uart_put_dec(s1 - s0);
    uart_puts(")\n");

    ok = (s1 > s0);
    check(ok, "phase A: rdcycle samples did not strictly increase");
    cks_feed(ok);
    ok = (s0 != 0);
    check(ok, "phase A: rdcycle sample 0 is zero (counter not running?)");
    cks_feed(ok);

    // Hand back to M-mode: the S-mode ecall (cause 9) is not
    // delegated, so it lands in the M-mode entry, which clears
    // mcounteren.CY and mrets into the phase-B driver.
    uart_puts("\nphase A done; handing back to M-mode to clear mcounteren.CY...\n");
    uart_drain();
    mcyu_s_ecall();

    // Unreachable: the M-mode handler mrets to mcyu_phase_b_start,
    // never back here.
    for (;;)
        __asm__ volatile("wfi");
}

// M-mode handoff handler, called from mcyu_trap.S on the single
// expected S-mode ecall. Clears mcounteren.CY (the gate under
// test), verifies the readbacks, and mrets with MPP=01 into the
// phase-B S-mode driver. Never returns to the trap entry.
void mcyu_m_handle(void) {
    unsigned long mcause = mcyu_m_regs[0];
    unsigned long mepc = mcyu_m_regs[1];
    unsigned long rb, sc;
    int ok;

    uart_puts("\n--- M-mode: phase-A handoff ecall ---\n");
    uart_puts("  mcause=");
    uart_put_hex(mcause);
    uart_puts(" mepc=");
    uart_put_hex(mepc);
    uart_puts("\n");

    ok = (mcause == 9);
    check(ok, "M-mode: trap is not an S-mode ecall (cause 9)");
    cks_feed(mcause);
    ok = (mepc == (unsigned long)mcyu_s_ecall_site);
    check(ok, "M-mode: mepc is not at the S-mode ecall site");
    cks_feed(ok);

    // The S-level gate must still be set: if scounteren.CY had
    // been lost, phase B would trap for the wrong reason.
    __asm__ volatile("csrr %0, scounteren" : "=r"(sc));
    uart_puts("  scounteren still=");
    uart_put_hex(sc);
    uart_puts("\n");
    ok = (sc == SCOUNTEREN_CY);
    check(ok, "M-mode: scounteren.CY did not stay set");
    cks_feed(ok);

    // The gate under test: clear mcounteren.CY and require the
    // 0 readback.
    __asm__ volatile("csrw mcounteren, %0" ::"r"(0UL) : "memory");
    __asm__ volatile("csrr %0, mcounteren" : "=r"(rb));
    uart_puts("  mcounteren after clear=");
    uart_put_hex(rb);
    uart_puts("\n");
    ok = (rb == 0);
    check(ok, "M-mode: mcounteren did not read back 0 after clear");
    cks_feed(ok);

    if (fails != 0) {
        uart_puts("RESULT: FAIL\n");
        uart_drain();
        for (;;)
            __asm__ volatile("wfi");
    }

    uart_puts("phase B armed (mcounteren.CY=0); dropping to S-mode...\n");
    uart_drain();

    // mret with MPP=01 (S-mode) into mcyu_phase_b_start.
    __asm__ volatile("la t0, mcyu_phase_b_start\n\t"
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

    // Unreachable: mret lands in the phase-B driver.
    for (;;)
        __asm__ volatile("wfi");
}

// Phase B driver, entered in S-mode via mret from the M-mode
// handler after mcounteren.CY was cleared.
void mcyu_phase_b_start(void) {
    uart_puts("\nin S-mode; mcounteren-cy-u-read phase B begins\n");
    uart_puts("dropping to U-mode with mcounteren.CY=0...\n");
    drop_to_umode((void (*)(void))mcyu_u_payload_b);
}

// Phase B continuation, entered in S-mode via sret from the
// dispatcher after the payload's SIG_B ecall.
void mcyu_phase_b_done(void) {
    int ok;

    uart_puts("\n--- back in S-mode: phase B complete ---\n");
    uart_puts("trap history:\n");
    print_trap(0, "SIG_A ecall   ");
    print_trap(1, "gated rdcycle ");
    print_trap(2, "SIG_B ecall   ");

    ok = (mcyu_hist_n == 3);
    check(ok, "phase B: trap count is not 3");
    cks_feed(ok);
    ok = (mcyu_hist[1][0] == 2);
    check(ok, "phase B: trap 2 scause is not 2 (illegal instruction)");
    cks_feed(mcyu_hist[1][0]);
    ok = (mcyu_hist[1][2] == mcyu_site_rdcycle_b);
    check(ok, "phase B: trap 2 sepc is not at the rdcycle site");
    cks_feed(ok);
    ok = (mcyu_hist[1][5] == SENTINEL);
    check(ok, "phase B: rdcycle destination changed (it retired?)");
    cks_feed(ok);
    ok = (mcyu_hist[2][0] == 8);
    check(ok, "phase B: trap 3 scause is not 8 (ecall from U-mode)");
    cks_feed(mcyu_hist[2][0]);
    ok = (mcyu_hist[2][2] == mcyu_site_ecall_b);
    check(ok, "phase B: trap 3 sepc is not at the payload ecall site");
    cks_feed(ok);
    ok = (mcyu_hist[2][6] == MCYU_SIG_B);
    check(ok, "phase B: trap 3 signal is not SIG_B");
    cks_feed(ok);

    uart_puts("\nchecksum (FNV-1a over verdict values) = ");
    uart_put_hex64(cksum);
    uart_puts("\n");

    uart_puts(fails == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    uart_drain();
    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;
    }
    for (;;) {
        __asm__ volatile("wfi");
    }
}

// Unexpected-trap continuation: any S-mode trap the dispatcher
// did not recognize (wrong cause, wrong signal, history
// overflow) lands here in S-mode. Report what arrived and park;
// the harness observes the FAIL as a timeout.
void mcyu_unexpected(void) {
    unsigned long i;

    uart_puts("\n--- UNEXPECTED TRAP ---\n");
    uart_puts("  cause=");
    uart_put_hex(mcyu_bad_cause);
    uart_puts(" signal(a0)=");
    uart_put_hex(mcyu_bad_sig);
    uart_puts("\n  history depth=");
    uart_put_dec(mcyu_hist_n);
    uart_puts("\n");
    for (i = 0; i < mcyu_hist_n && i < MCYU_HIST_N; i++)
        print_trap((int)i, "hist");
    uart_puts("RESULT: FAIL\n");
    uart_drain();
    for (;;) {
        __asm__ volatile("wfi");
    }
}

// S-mode payload driver, entered via mret with MPP=01.
void mcyu_smode_test(void) {
    uart_puts("in S-mode; mcounteren-cy-u-read experiment begins\n");
    uart_puts("phase A payload: rdcycle site ");
    uart_put_hex(mcyu_site_rdcycle_a);
    uart_puts(", ecall site ");
    uart_put_hex(mcyu_site_ecall_a);
    uart_puts("\n");
    uart_puts("dropping to U-mode with mcounteren.CY=1...\n");
    drop_to_umode((void (*)(void))mcyu_u_payload_a);
}

static unsigned long read_mcounteren(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mcounteren" : "=r"(v));
    return v;
}

static void write_mcounteren(unsigned long v) {
    __asm__ volatile("csrw mcounteren, %0" ::"r"(v));
}

static unsigned long read_scounteren(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, scounteren" : "=r"(v));
    return v;
}

static void write_scounteren(unsigned long v) {
    __asm__ volatile("csrw scounteren, %0" ::"r"(v));
}

int main(void) {
    unsigned long boot_mc, rb, deleg;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("mcounteren.CY gates U-mode rdcycle\n");
    uart_puts("CY set must read, CY clear must trap\n");
    uart_puts("========================================\n\n");

    // 1. Boot mcounteren and the writability probe: write 0x7
    // (CY|TM|IR), read back; the readback must be exactly 0x7
    // so the later gate writes are proven to take effect
    // rather than be silently ignored.
    boot_mc = read_mcounteren();
    uart_puts("boot: mcounteren=");
    uart_put_hex(boot_mc);
    uart_puts("\n");
    check(boot_mc == 0, "boot mcounteren is not 0");
    cks_feed(boot_mc);

    write_mcounteren(0x7UL);
    rb = read_mcounteren();
    uart_puts("probe: csrw mcounteren, 0x7; readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == 0x7UL, "mcounteren write of 0x7 did not read back as 0x7");
    cks_feed(rb);

    // 2. Gate arm: write mcounteren = 0x1 (CY set), read back.
    write_mcounteren(MCOUNTEREN_CY);
    rb = read_mcounteren();
    uart_puts("write: csrw mcounteren, 0x1 (CY); readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check((rb & MCOUNTEREN_CY) != 0, "mcounteren.CY did not stick");
    cks_feed((rb & MCOUNTEREN_CY) != 0);

    // 3. Hold the S-level gate open: scounteren.CY = 1 for the
    // whole run, so it can never mask the M-level gate under
    // test.
    write_scounteren(SCOUNTEREN_CY);
    rb = read_scounteren();
    uart_puts("write: csrw scounteren, 0x1 (CY); readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == SCOUNTEREN_CY, "scounteren.CY did not stick");
    cks_feed(rb);

    // 4. Trap vectors, delegation, PMP.
    __asm__ volatile("la t0, mcyu_mt_trap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mcyu_trap_entry\n\t"
                     "csrw stvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mcyu_regs\n\t"
                     "csrw sscratch, t0"
                     :
                     :
                     : "t0", "memory");
    // Delegate the illegal-instruction trap (bit 2) and the
    // U-mode ecall (bit 8) to S-mode; the S-mode ecall (bit 9)
    // stays in M-mode as the phase handoff. All other traps
    // stay in M-mode and take the FAIL path.
    __asm__ volatile("li t0, 0x104\n\t"
                     "csrw medeleg, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("csrr %0, medeleg" : "=r"(deleg));
    uart_puts("medeleg readback=");
    uart_put_hex(deleg);
    uart_puts("\n");
    check((deleg & (MEDELEG_ILLEGAL_INSN | MEDELEG_ECALL_U)) ==
              (MEDELEG_ILLEGAL_INSN | MEDELEG_ECALL_U),
          "medeleg bits 2 and 8 not set");
    cks_feed((deleg & (MEDELEG_ILLEGAL_INSN | MEDELEG_ECALL_U)) ==
             (MEDELEG_ILLEGAL_INSN | MEDELEG_ECALL_U));

    // PMP: with no PMP entry programmed, U-mode has no access
    // to any address. Open the whole address space with one
    // NAPOT R/W/X entry before the drops; without this the
    // first U-mode instruction fetch raises an instruction
    // access fault.
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t" // A=NAPOT, R/W/X, unlocked
                     "csrw pmpcfg0, t0\n\t"
                     :
                     :
                     : "t0", "memory");

    // Capture the payload site addresses before the drop.
    mcyu_site_rdcycle_a = (unsigned long)mcyu_u_rdcycle_a;
    mcyu_site_ecall_a = (unsigned long)mcyu_u_ecall_a;
    mcyu_site_rdcycle_b = (unsigned long)mcyu_u_rdcycle_b;
    mcyu_site_ecall_b = (unsigned long)mcyu_u_ecall_b;

    uart_puts("setup complete; dropping to S-mode...\n");
    uart_drain();

    // 5. mret with MPP=01 (S-mode) into mcyu_smode_test. The
    // whole experiment runs from there.
    __asm__ volatile("la t0, mcyu_smode_test\n\t"
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

    // Unreachable: mret lands in the S-mode driver, which
    // finishes via the test-device finisher or parks on
    // failure.
    for (;;)
        __asm__ volatile("wfi");
}
