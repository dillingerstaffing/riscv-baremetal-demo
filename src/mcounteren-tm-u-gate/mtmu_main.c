// mtmu_main.c: mcounteren.TM gating of U-mode rdtime (backlog
// item mcounteren-tm-u-gate; the backlog title's bit-0 note is a
// typo, TM is bit 1, bit 0 is CY).
//
// Mechanism under test: mcounteren.TM is the M-level gate for the
// time CSR. With scounteren.TM held set, clearing mcounteren.TM
// must turn a U-mode rdtime into an illegal-instruction trap
// (scause = 2, destination register untouched); with TM set, the
// same U-mode rdtime must succeed and return an advancing count.
// This is the converse of the done scounteren-tm-gate module,
// which held mcounteren.TM set and toggled the S-level gate;
// here the S-level gate is held set and the M-level gate is
// toggled. It is also the M-level counterpart of
// mcounteren-time-gate, which gated S-mode reads; none of the
// done modules observed the M-level gate's effect on a U-mode
// read.
//
// One subtlety the setup must get right: a U-mode rdtime is
// gated by BOTH mcounteren.TM (M-level) and scounteren.TM
// (S-level), and QEMU resets both to 0. If scounteren.TM stayed
// clear, phases A and C would trap regardless of mcounteren.TM
// and prove nothing. M-mode therefore sets scounteren.TM during
// setup and it is never cleared afterwards, so mcounteren.TM is
// the only gate under test.
//
// The run, on the QEMU virt board:
//   M-mode boot:
//     1. Record the boot mcounteren value (QEMU resets it to 0;
//        the restore step writes this exact value back) and the
//        boot scounteren/medeleg values; probe writability by
//        writing 0x7 (CY|TM|IR) to mcounteren and requiring the
//        readback to be exactly 0x7, so the later gate writes
//        are proven to take effect rather than be silently
//        ignored.
//     2. Write mcounteren = 0x2 (TM set) and require the
//        readback to carry TM.
//     3. Write scounteren = 0x2 (TM set) and require the
//        readback to carry TM, so the S-level gate never masks
//        the M-level gate under test.
//     4. Install the M-mode trap entry (handles exactly three
//        expected S-mode ecalls between the phases, tracked by
//        mtmu_m_phase: after phase A it verifies mcause/mepc,
//        requires scounteren to still read 0x2, clears
//        mcounteren.TM, requires the 0 readback, and mrets with
//        MPP=01 into the phase-B S-mode driver; after phase B
//        it sets mcounteren.TM again, requires the 0x2
//        readback, and mrets into the phase-C driver; after
//        phase C it restores mcounteren, scounteren, and
//        medeleg to their boot values with readbacks, requires
//        the M-mode trap count to be exactly 3, and on PASS
//        writes the test-device finisher; any other M-mode
//        trap or any phase-skipping order takes the FAIL path),
//        the S-mode stvec handler (records scause/stval/sepc
//        and the interrupted t0/a0/t1/s0, appends each trap to
//        a history, and resumes where the C dispatcher
//        decides), point sscratch at the S-mode save area, set
//        medeleg bits 2 and 8 so the illegal-instruction trap
//        and the U-mode ecall are delivered to S-mode (S-mode
//        ecalls stay in M-mode), and open a whole-address-space
//        PMP NAPOT entry (U-mode is default-deny without one).
//     5. mret with MPP=01 into mtmu_smode_test.
//   S-mode driver:
//     A. sret with SPP=0 into the U-mode payload A with
//        mcounteren.TM=1. Expected: no trap from either
//        rdtime; the payload's ecall (cause 8, signal SIG_A)
//        carries the two samples in the interrupted t0/t1. The
//        dispatcher answers by resuming in S-mode at
//        mtmu_phase_a_done, which requires strictly increasing
//        samples and a nonzero first sample, then issues an
//        S-mode ecall (cause 9, not delegated) to hand back to
//        M-mode.
//     B. The M-mode handler clears mcounteren.TM, requires the
//        0 readback, requires scounteren to still read 0x2,
//        and mrets with MPP=01 into mtmu_phase_b_start, which
//        srets with SPP=0 into the U-mode payload B. Expected:
//        exactly two more S-mode traps. Trap 2 is the gated
//        rdtime: scause = 2, sepc exactly at the rdtime site,
//        and the interrupted t0 still holding its pre-fault
//        sentinel (the rdtime never retired). Trap 3 is the
//        payload's ecall (cause 8, signal SIG_B), which the
//        dispatcher answers by resuming in S-mode at
//        mtmu_phase_b_done.
//     C. The M-mode handler sets mcounteren.TM again, requires
//        the 0x2 readback, requires scounteren to still read
//        0x2, and mrets with MPP=01 into mtmu_phase_c_start,
//        which srets with SPP=0 into the U-mode payload C.
//        Expected: no trap from either rdtime; the payload's
//        ecall (cause 8, signal SIG_C) carries the two samples
//        in the interrupted t0/t1. mtmu_phase_c_done requires
//        strictly increasing samples and a nonzero first
//        sample, prints the checksum, then issues an S-mode
//        ecall to hand back to M-mode for the restore.
//   Verdict: RESULT: PASS only if all 38 checks held. On PASS
//   the restored CSRs are published and the virt test-device
//   finisher word shuts the machine down (QEMU exits 0). On
//   FAIL the hart parks in a wfi loop; the bench harness runs
//   QEMU under timeout, so a FAIL is observable as exit status
//   124.
//
// The fault and signal sites use assembler-resolved global
// labels in mtmu_trap.S (never C &&label: the toolchain
// miscompiles labels-as-values at -O2, so &&label is never used
// for trap-resume addresses).

#include "../uart.h"

extern void mtmu_trap_entry(void);
extern void mtmu_mt_trap_entry(void);
extern void mtmu_s_ecall(void);

// U-mode payload sites, defined in mtmu_trap.S.
extern char mtmu_u_payload_a[];
extern char mtmu_u_rdtime_a[];
extern char mtmu_u_ecall_a[];
extern char mtmu_u_payload_b[];
extern char mtmu_u_rdtime_b[];
extern char mtmu_u_ecall_b[];
extern char mtmu_u_payload_c[];
extern char mtmu_u_rdtime_c[];
extern char mtmu_u_ecall_c[];
extern char mtmu_s_ecall_site[];

// S-mode continuations, entered via sret from the dispatcher or
// via mret from the M-mode handler.
void mtmu_smode_test(void);
void mtmu_phase_a_done(void);
void mtmu_phase_b_start(void);
void mtmu_phase_b_done(void);
void mtmu_phase_c_start(void);
void mtmu_phase_c_done(void);
void mtmu_unexpected(void);

// M-mode handoff handler, called from mtmu_trap.S on each of
// the three expected S-mode ecalls. Arms the next phase or
// restores the CSRs and exits; never returns.
void mtmu_m_handle(void);

// S-mode trap record, written by mtmu_trap.S:
// [0] scause, [1] stval, [2] sepc at entry, [3] trap count,
// [4] resume pc chosen by the dispatcher, [5] interrupted t0,
// [6] interrupted a0, [7] interrupted t1, [8] interrupted s0.
volatile unsigned long mtmu_regs[10];

// M-mode trap record, written by mtmu_trap.S: [0] mcause,
// [1] mepc at entry, [2] M-mode trap count.
volatile unsigned long mtmu_m_regs[3];

// M-mode phase tracker: 0 = expect the post-phase-A handoff,
// 1 = expect the post-phase-B handoff, 2 = expect the post-
// phase-C handoff. The handler mrets to a different S-mode
// driver per value; any other value takes the FAIL path.
volatile unsigned long mtmu_m_phase;

// Trap history: the dispatcher copies every S-mode trap's
// record here so the phase-B checks can still inspect the gated
// rdtime trap after the closing ecall trap overwrote mtmu_regs.
#define MTMU_HIST_N 8
volatile unsigned long mtmu_hist[MTMU_HIST_N][10];
volatile unsigned long mtmu_hist_n;
volatile unsigned long mtmu_bad_cause;
volatile unsigned long mtmu_bad_sig;

// Site addresses, captured in M-mode before the first drop.
volatile unsigned long mtmu_site_rdtime_a;
volatile unsigned long mtmu_site_ecall_a;
volatile unsigned long mtmu_site_rdtime_b;
volatile unsigned long mtmu_site_ecall_b;
volatile unsigned long mtmu_site_rdtime_c;
volatile unsigned long mtmu_site_ecall_c;

// Boot CSR values, captured in M-mode before any write, for the
// restore step.
volatile unsigned long mtmu_boot_mc;
volatile unsigned long mtmu_boot_sc;
volatile unsigned long mtmu_boot_deleg;

#define MTMU_SIG_A 0xa1a1UL
#define MTMU_SIG_B 0xa2a2UL
#define MTMU_SIG_C 0xa3a3UL

#define SENTINEL 0xDEADBEEFDEADBEEFUL
#define MCOUNTEREN_TM (1UL << 1)
#define SCOUNTEREN_TM (1UL << 1)
#define MEDELEG_ILLEGAL_INSN (1UL << 2)
#define MEDELEG_ECALL_U (1UL << 8)

// FNV-1a (64-bit) over the verdict-relevant values, fed in a
// fixed order from every privilege level. Only run-invariant
// values are fed (causes, counts, check booleans), never raw
// timer samples, so the checksum is identical on every passing
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

// mret with MPP=01 into an S-mode driver. Never returns.
static void drop_to_smode(void (*driver)(void)) {
    __asm__ volatile("la t0, 0f\n\t"
                     "csrw mepc, %0\n\t"
                     "csrr t0, mstatus\n\t"
                     "li t1, 0x1800\n\t"   // clear MPP bits 12:11
                     "not t1, t1\n\t"
                     "and t0, t0, t1\n\t"
                     "li t1, 0x800\n\t"    // MPP = 01 (S-mode)
                     "or t0, t0, t1\n\t"
                     "csrw mstatus, t0\n\t"
                     "mret\n\t"
                     "0:\n\t"
                     ::"r"(driver)
                     : "t0", "t1", "memory");
    for (;;)
        __asm__ volatile("wfi");
}

// C dispatcher, called from mtmu_trap.S with r = mtmu_regs.
// Appends the trap record to the history and returns the resume
// pc: sepc+4 for the gated rdtime (illegal instruction), or an
// S-mode continuation for the U-mode ecall signals (setting
// sstatus.SPP=1 first so sret resumes in S-mode).
unsigned long mtmu_handle(volatile unsigned long *r) {
    unsigned long cause = r[0];
    unsigned long resume;
    int i;

    if (mtmu_hist_n < MTMU_HIST_N) {
        for (i = 0; i < 9; i++)
            mtmu_hist[mtmu_hist_n][i] = r[i];
        mtmu_hist_n++;
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
        if (sig == MTMU_SIG_A)
            resume = (unsigned long)mtmu_phase_a_done;
        else if (sig == MTMU_SIG_B)
            resume = (unsigned long)mtmu_phase_b_done;
        else if (sig == MTMU_SIG_C)
            resume = (unsigned long)mtmu_phase_c_done;
        else {
            mtmu_bad_cause = cause;
            mtmu_bad_sig = sig;
            resume = (unsigned long)mtmu_unexpected;
        }
        r[4] = resume;
        return resume;
    }
    mtmu_bad_cause = cause;
    mtmu_bad_sig = r[6];
    __asm__ volatile("li t0, 0x100\n\t"
                     "csrs sstatus, t0"
                     ::: "t0", "memory");
    resume = (unsigned long)mtmu_unexpected;
    r[4] = resume;
    return resume;
}

static void print_trap(int idx, const char *tag) {
    volatile unsigned long *h = mtmu_hist[idx];
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
void mtmu_phase_a_done(void) {
    unsigned long s0, s1;
    int ok;

    uart_puts("\n--- back in S-mode: phase A complete ---\n");
    uart_puts("trap history:\n");
    print_trap(0, "SIG_A ecall ");

    ok = (mtmu_hist_n == 1);
    check(ok, "phase A: trap count is not 1");
    cks_feed(ok);
    ok = (mtmu_hist[0][0] == 8);
    check(ok, "phase A: trap 1 scause is not 8 (ecall from U-mode)");
    cks_feed(mtmu_hist[0][0]);
    ok = (mtmu_hist[0][2] == mtmu_site_ecall_a);
    check(ok, "phase A: trap 1 sepc is not at the payload ecall site");
    cks_feed(ok);
    ok = (mtmu_hist[0][6] == MTMU_SIG_A);
    check(ok, "phase A: trap 1 signal is not SIG_A");
    cks_feed(ok);

    s0 = mtmu_hist[0][5];
    s1 = mtmu_hist[0][7];
    uart_puts("  rdtime sample 0 (t0) = ");
    uart_put_hex(s0);
    uart_puts("\n  rdtime sample 1 (t1) = ");
    uart_put_hex(s1);
    uart_puts("  (delta = ");
    uart_put_dec(s1 - s0);
    uart_puts(")\n");

    ok = (s1 > s0);
    check(ok, "phase A: rdtime samples did not strictly increase");
    cks_feed(ok);
    ok = (s0 != 0);
    check(ok, "phase A: rdtime sample 0 is zero (timer not running?)");
    cks_feed(ok);

    // Hand back to M-mode: the S-mode ecall (cause 9) is not
    // delegated, so it lands in the M-mode entry, which clears
    // mcounteren.TM and mrets into the phase-B driver.
    uart_puts("\nphase A done; handing back to M-mode to clear mcounteren.TM...\n");
    uart_drain();
    mtmu_s_ecall();

    // Unreachable: the M-mode handler mrets to mtmu_phase_b_start,
    // never back here.
    for (;;)
        __asm__ volatile("wfi");
}

// M-mode handoff handler, called from mtmu_trap.S on each of
// the three expected S-mode ecalls. Tracks the phase in
// mtmu_m_phase: 0 clears mcounteren.TM (arms phase B), 1 sets it
// again (arms phase C), 2 restores the boot CSR values, prints
// the verdict, and exits via the test-device finisher on PASS.
// Never returns to the trap entry.
void mtmu_m_handle(void) {
    unsigned long mcause = mtmu_m_regs[0];
    unsigned long mepc = mtmu_m_regs[1];
    unsigned long rb, sc, m_n;
    int ok;

    uart_puts("\n--- M-mode: S-mode handoff ecall ---\n");
    uart_puts("  mcause=");
    uart_put_hex(mcause);
    uart_puts(" mepc=");
    uart_put_hex(mepc);
    uart_puts(" M-mode trap count=");
    uart_put_dec(mtmu_m_regs[2]);
    uart_puts(" phase=");
    uart_put_dec(mtmu_m_phase);
    uart_puts("\n");

    ok = (mcause == 9);
    check(ok, "M-mode: trap is not an S-mode ecall (cause 9)");
    cks_feed(mcause);
    ok = (mepc == (unsigned long)mtmu_s_ecall_site);
    check(ok, "M-mode: mepc is not at the S-mode ecall site");
    cks_feed(ok);

    if (mtmu_m_phase == 0) {
        // The S-level gate must still be set: if scounteren.TM
        // had been lost, phase B would trap for the wrong
        // reason.
        __asm__ volatile("csrr %0, scounteren" : "=r"(sc));
        uart_puts("  scounteren still=");
        uart_put_hex(sc);
        uart_puts("\n");
        ok = (sc == SCOUNTEREN_TM);
        check(ok, "M-mode: scounteren.TM did not stay set");
        cks_feed(ok);

        // The gate under test: clear mcounteren.TM and require
        // the readback to carry no TM bit.
        __asm__ volatile("csrw mcounteren, %0" ::"r"(0UL) : "memory");
        __asm__ volatile("csrr %0, mcounteren" : "=r"(rb));
        uart_puts("  mcounteren after clear=");
        uart_put_hex(rb);
        uart_puts("\n");
        ok = ((rb & MCOUNTEREN_TM) == 0);
        check(ok, "M-mode: mcounteren.TM did not clear");
        cks_feed(ok);

        if (fails != 0) {
            uart_puts("RESULT: FAIL\n");
            uart_drain();
            for (;;)
                __asm__ volatile("wfi");
        }

        mtmu_m_phase = 1;
        uart_puts("phase B armed (mcounteren.TM=0); dropping to S-mode...\n");
        uart_drain();
        drop_to_smode(mtmu_phase_b_start);
    } else if (mtmu_m_phase == 1) {
        // scounteren.TM must have stayed set through phase B:
        // the gated read only proves the M-level gate when the
        // S-level gate was open.
        __asm__ volatile("csrr %0, scounteren" : "=r"(sc));
        uart_puts("  scounteren still=");
        uart_put_hex(sc);
        uart_puts("\n");
        ok = (sc == SCOUNTEREN_TM);
        check(ok, "M-mode: scounteren.TM did not stay set through phase B");
        cks_feed(ok);

        // Restore the gate: set mcounteren.TM again and require
        // the readback.
        __asm__ volatile("csrw mcounteren, %0" ::"r"(MCOUNTEREN_TM) : "memory");
        __asm__ volatile("csrr %0, mcounteren" : "=r"(rb));
        uart_puts("  mcounteren after set=");
        uart_put_hex(rb);
        uart_puts("\n");
        ok = ((rb & MCOUNTEREN_TM) != 0);
        check(ok, "M-mode: mcounteren.TM did not set");
        cks_feed(ok);

        if (fails != 0) {
            uart_puts("RESULT: FAIL\n");
            uart_drain();
            for (;;)
                __asm__ volatile("wfi");
        }

        mtmu_m_phase = 2;
        uart_puts("phase C armed (mcounteren.TM=1); dropping to S-mode...\n");
        uart_drain();
        drop_to_smode(mtmu_phase_c_start);
    } else if (mtmu_m_phase == 2) {
        // Restore the CSRs to their boot values before exit.
        __asm__ volatile("csrw mcounteren, %0" ::"r"(mtmu_boot_mc) : "memory");
        __asm__ volatile("csrr %0, mcounteren" : "=r"(rb));
        uart_puts("  mcounteren restore readback=");
        uart_put_hex(rb);
        uart_puts("\n");
        ok = (rb == mtmu_boot_mc);
        check(ok, "M-mode: mcounteren did not restore to its boot value");
        cks_feed(ok);

        __asm__ volatile("csrw scounteren, %0" ::"r"(mtmu_boot_sc) : "memory");
        __asm__ volatile("csrr %0, scounteren" : "=r"(sc));
        uart_puts("  scounteren restore readback=");
        uart_put_hex(sc);
        uart_puts("\n");
        ok = (sc == mtmu_boot_sc);
        check(ok, "M-mode: scounteren did not restore to its boot value");
        cks_feed(ok);

        __asm__ volatile("csrw medeleg, %0" ::"r"(mtmu_boot_deleg) : "memory");
        __asm__ volatile("csrr %0, medeleg" : "=r"(rb));
        uart_puts("  medeleg restore readback=");
        uart_put_hex(rb);
        uart_puts("\n");
        ok = (rb == mtmu_boot_deleg);
        check(ok, "M-mode: medeleg did not restore to its boot value");
        cks_feed(ok);

        m_n = mtmu_m_regs[2];
        ok = (m_n == 3);
        check(ok, "M-mode: trap count is not exactly the 3 expected handoffs");
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

    // Unreachable phases (handler re-entered out of order):
    // report and park.
    uart_puts("\n--- UNEXPECTED M-MODE PHASE ---\n");
    uart_puts("  phase=");
    uart_put_dec(mtmu_m_phase);
    uart_puts("\nRESULT: FAIL\n");
    uart_drain();
    for (;;)
        __asm__ volatile("wfi");
}

// Phase B driver, entered in S-mode via mret from the M-mode
// handler after mcounteren.TM was cleared.
void mtmu_phase_b_start(void) {
    uart_puts("\nin S-mode; mcounteren-tm-u-gate phase B begins\n");
    uart_puts("dropping to U-mode with mcounteren.TM=0...\n");
    drop_to_umode((void (*)(void))mtmu_u_payload_b);
}

// Phase B continuation, entered in S-mode via sret from the
// dispatcher after the payload's SIG_B ecall.
void mtmu_phase_b_done(void) {
    int ok;

    uart_puts("\n--- back in S-mode: phase B complete ---\n");
    uart_puts("trap history:\n");
    print_trap(0, "SIG_A ecall   ");
    print_trap(1, "gated rdtime  ");
    print_trap(2, "SIG_B ecall   ");

    ok = (mtmu_hist_n == 3);
    check(ok, "phase B: trap count is not 3");
    cks_feed(ok);
    ok = (mtmu_hist[1][0] == 2);
    check(ok, "phase B: trap 2 scause is not 2 (illegal instruction)");
    cks_feed(mtmu_hist[1][0]);
    ok = (mtmu_hist[1][2] == mtmu_site_rdtime_b);
    check(ok, "phase B: trap 2 sepc is not at the rdtime site");
    cks_feed(ok);
    ok = (mtmu_hist[1][5] == SENTINEL);
    check(ok, "phase B: rdtime destination changed (it retired?)");
    cks_feed(ok);
    ok = (mtmu_hist[2][0] == 8);
    check(ok, "phase B: trap 3 scause is not 8 (ecall from U-mode)");
    cks_feed(mtmu_hist[2][0]);
    ok = (mtmu_hist[2][2] == mtmu_site_ecall_b);
    check(ok, "phase B: trap 3 sepc is not at the payload ecall site");
    cks_feed(ok);
    ok = (mtmu_hist[2][6] == MTMU_SIG_B);
    check(ok, "phase B: trap 3 signal is not SIG_B");
    cks_feed(ok);

    // Hand back to M-mode: the S-mode ecall (cause 9) is not
    // delegated, so it lands in the M-mode entry, which sets
    // mcounteren.TM again and mrets into the phase-C driver.
    uart_puts("\nphase B done; handing back to M-mode to set mcounteren.TM...\n");
    uart_drain();
    mtmu_s_ecall();

    // Unreachable: the M-mode handler mrets to mtmu_phase_c_start,
    // never back here.
    for (;;)
        __asm__ volatile("wfi");
}

// Phase C driver, entered in S-mode via mret from the M-mode
// handler after mcounteren.TM was set again.
void mtmu_phase_c_start(void) {
    uart_puts("\nin S-mode; mcounteren-tm-u-gate phase C begins\n");
    uart_puts("dropping to U-mode with mcounteren.TM=1...\n");
    drop_to_umode((void (*)(void))mtmu_u_payload_c);
}

// Phase C continuation, entered in S-mode via sret from the
// dispatcher after the payload's SIG_C ecall.
void mtmu_phase_c_done(void) {
    unsigned long s0, s1;
    int ok;

    uart_puts("\n--- back in S-mode: phase C complete ---\n");
    uart_puts("trap history:\n");
    print_trap(0, "SIG_A ecall   ");
    print_trap(1, "gated rdtime  ");
    print_trap(2, "SIG_B ecall   ");
    print_trap(3, "SIG_C ecall   ");

    ok = (mtmu_hist_n == 4);
    check(ok, "phase C: trap count is not 4");
    cks_feed(ok);
    ok = (mtmu_hist[3][0] == 8);
    check(ok, "phase C: trap 4 scause is not 8 (ecall from U-mode)");
    cks_feed(mtmu_hist[3][0]);
    ok = (mtmu_hist[3][2] == mtmu_site_ecall_c);
    check(ok, "phase C: trap 4 sepc is not at the payload ecall site");
    cks_feed(ok);
    ok = (mtmu_hist[3][6] == MTMU_SIG_C);
    check(ok, "phase C: trap 4 signal is not SIG_C");
    cks_feed(ok);

    s0 = mtmu_hist[3][5];
    s1 = mtmu_hist[3][7];
    uart_puts("  rdtime sample 0 (t0) = ");
    uart_put_hex(s0);
    uart_puts("\n  rdtime sample 1 (t1) = ");
    uart_put_hex(s1);
    uart_puts("  (delta = ");
    uart_put_dec(s1 - s0);
    uart_puts(")\n");

    ok = (s1 > s0);
    check(ok, "phase C: rdtime samples did not strictly increase");
    cks_feed(ok);
    ok = (s0 != 0);
    check(ok, "phase C: rdtime sample 0 is zero (timer not running?)");
    cks_feed(ok);

    // Hand back to M-mode: the S-mode ecall (cause 9) is not
    // delegated, so it lands in the M-mode entry, which restores
    // the CSRs to their boot values and exits.
    uart_puts("\nphase C done; handing back to M-mode to restore CSRs and exit...\n");
    uart_drain();
    mtmu_s_ecall();

    // Unreachable: the M-mode handler restores the CSRs and
    // writes the finisher or parks, never back here.
    for (;;)
        __asm__ volatile("wfi");
}

// Unexpected-trap continuation: any S-mode trap the dispatcher
// did not recognize (wrong cause, wrong signal, history
// overflow) lands here in S-mode. Report what arrived and park;
// the harness observes the FAIL as a timeout.
void mtmu_unexpected(void) {
    unsigned long i;

    uart_puts("\n--- UNEXPECTED TRAP ---\n");
    uart_puts("  cause=");
    uart_put_hex(mtmu_bad_cause);
    uart_puts(" signal(a0)=");
    uart_put_hex(mtmu_bad_sig);
    uart_puts("\n  history depth=");
    uart_put_dec(mtmu_hist_n);
    uart_puts("\n");
    for (i = 0; i < mtmu_hist_n && i < MTMU_HIST_N; i++)
        print_trap((int)i, "hist");
    uart_puts("RESULT: FAIL\n");
    uart_drain();
    for (;;) {
        __asm__ volatile("wfi");
    }
}

// S-mode payload driver, entered via mret with MPP=01.
void mtmu_smode_test(void) {
    uart_puts("in S-mode; mcounteren-tm-u-gate experiment begins\n");
    uart_puts("phase A payload: rdtime site ");
    uart_put_hex(mtmu_site_rdtime_a);
    uart_puts(", ecall site ");
    uart_put_hex(mtmu_site_ecall_a);
    uart_puts("\n");
    uart_puts("dropping to U-mode with mcounteren.TM=1...\n");
    drop_to_umode((void (*)(void))mtmu_u_payload_a);
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
    unsigned long rb, deleg;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("mcounteren.TM gates U-mode rdtime\n");
    uart_puts("TM set must read, TM clear must trap\n");
    uart_puts("========================================\n\n");

    // 1. Boot CSR values and the writability probe: record the
    // boot mcounteren (QEMU resets it to 0; the restore step
    // writes this exact value back), boot scounteren, and boot
    // medeleg; write 0x7 (CY|TM|IR) to mcounteren and require
    // the readback to be exactly 0x7, so the later gate writes
    // are proven to take effect rather than be silently
    // ignored.
    mtmu_boot_mc = read_mcounteren();
    mtmu_boot_sc = read_scounteren();
    uart_puts("boot: mcounteren=");
    uart_put_hex(mtmu_boot_mc);
    uart_puts(" scounteren=");
    uart_put_hex(mtmu_boot_sc);
    uart_puts("\n");
    check(mtmu_boot_mc == 0, "boot mcounteren is not 0");
    cks_feed(mtmu_boot_mc);

    write_mcounteren(0x7UL);
    rb = read_mcounteren();
    uart_puts("probe: csrw mcounteren, 0x7; readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == 0x7UL, "mcounteren write of 0x7 did not read back as 0x7");
    cks_feed(rb);

    // 2. Gate arm: write mcounteren = 0x2 (TM set), read back.
    write_mcounteren(MCOUNTEREN_TM);
    rb = read_mcounteren();
    uart_puts("write: csrw mcounteren, 0x2 (TM); readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check((rb & MCOUNTEREN_TM) != 0, "mcounteren.TM did not stick");
    cks_feed((rb & MCOUNTEREN_TM) != 0);

    // 3. Hold the S-level gate open: scounteren.TM = 1 for the
    // whole run, so it can never mask the M-level gate under
    // test.
    write_scounteren(SCOUNTEREN_TM);
    rb = read_scounteren();
    uart_puts("write: csrw scounteren, 0x2 (TM); readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == SCOUNTEREN_TM, "scounteren.TM did not stick");
    cks_feed(rb);

    // 4. Trap vectors, delegation, PMP.
    __asm__ volatile("la t0, mtmu_mt_trap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mtmu_trap_entry\n\t"
                     "csrw stvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, mtmu_regs\n\t"
                     "csrw sscratch, t0"
                     :
                     :
                     : "t0", "memory");
    // Delegate the illegal-instruction trap (bit 2) and the
    // U-mode ecall (bit 8) to S-mode; the S-mode ecall (bit 9)
    // stays in M-mode as the phase handoff. All other traps
    // stay in M-mode and take the FAIL path. Record the boot
    // medeleg before writing, for the restore step.
    __asm__ volatile("csrr %0, medeleg" : "=r"(mtmu_boot_deleg));
    __asm__ volatile("li t0, 0x104\n\t"
                     "csrw medeleg, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("csrr %0, medeleg" : "=r"(deleg));
    uart_puts("boot: medeleg=");
    uart_put_hex(mtmu_boot_deleg);
    uart_puts("\n");
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
    mtmu_site_rdtime_a = (unsigned long)mtmu_u_rdtime_a;
    mtmu_site_ecall_a = (unsigned long)mtmu_u_ecall_a;
    mtmu_site_rdtime_b = (unsigned long)mtmu_u_rdtime_b;
    mtmu_site_ecall_b = (unsigned long)mtmu_u_ecall_b;
    mtmu_site_rdtime_c = (unsigned long)mtmu_u_rdtime_c;
    mtmu_site_ecall_c = (unsigned long)mtmu_u_ecall_c;

    uart_puts("setup complete; dropping to S-mode...\n");
    uart_drain();

    // 5. mret with MPP=01 (S-mode) into mtmu_smode_test. The
    // whole experiment runs from there.
    __asm__ volatile("la t0, mtmu_smode_test\n\t"
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
