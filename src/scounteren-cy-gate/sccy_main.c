// sccy_main.c: scounteren.CY gating of U-mode rdcycle (backlog
// item scounteren-cy-gate).
//
// Mechanism under test: the scounteren CSR gates the cycle CSR
// for U-mode. With scounteren.CY clear, a U-mode rdcycle must
// raise an illegal-instruction exception (scause = 2) instead of
// returning the cycle count; with CY set, the same U-mode
// rdcycle must succeed and return an advancing count. The gate
// only applies below M-mode, so the experiment needs two real
// privilege drops (M -> S -> U) to observe it.
//
// One subtlety the setup must get right: mcounteren.CY also
// gates the cycle CSR for S-mode and U-mode, and QEMU resets
// mcounteren to 0. If mcounteren.CY stayed clear, the U-mode
// rdcycle would trap in phase B too and the scounteren.CY write
// would prove nothing. M-mode therefore sets mcounteren.CY
// during setup, so scounteren.CY is the only gate under test.
//
// The run, on the QEMU virt board:
//   M-mode boot:
//     1. Record the boot scounteren value; probe writability by
//        writing 0x7 (CY|TM|IR) and requiring the readback to be
//        exactly 0x7, so the later 0 write is proven to take
//        effect rather than be silently ignored.
//     2. Write scounteren = 0 and require the readback to be 0.
//     3. Set mcounteren.CY (0x1) and require the readback to
//        carry the bit, for the reason above.
//     4. Install the M-mode park handler (any trap reaching
//        M-mode is unexpected), the S-mode stvec handler
//        (records scause/stval/sepc and the interrupted
//        t0/a0/t1/s0, appends each trap to a history, and
//        resumes where the C dispatcher decides), point
//        sscratch at the S-mode save area, set medeleg bits 2
//        and 8 so the illegal-instruction trap and the U-mode
//        ecall are delivered to S-mode (all other traps stay
//        in M-mode), and open a whole-address-space PMP NAPOT
//        entry (U-mode is default-deny without one).
//     5. mret with MPP=01 into sccy_smode_test.
//   S-mode payload:
//     A. sret with SPP=0 into the U-mode payload A with CY
//        clear. Expected: exactly two S-mode traps. Trap 1 is
//        the gated rdcycle: scause = 2, sepc exactly at the
//        rdcycle site, and the interrupted t0 still holding its
//        pre-fault sentinel (the rdcycle never retired). Trap 2
//        is the payload's ecall (cause 8, signal SIG_A in a0),
//        which the dispatcher answers by resuming in S-mode at
//        sccy_phase_a_done.
//     B. S-mode sets scounteren.CY, requires the readback to
//        carry the bit, and srets with SPP=0 into the U-mode
//        payload B. Expected: no new trap from either rdcycle,
//        then the payload's ecall (cause 8, SIG_B) carrying
//        the two samples in the interrupted t0/t1. The second
//        sample is re-read in a bounded spin until it differs
//        from the first, because back-to-back reads can land
//        in the same host tick; the verdict needs strictly
//        increasing samples. S-mode requires sample1 >
//        sample0 and sample0 != 0.
//   Verdict: RESULT: PASS only if all 18 checks held. On PASS
//   the virt test-device finisher word shuts the machine down
//   (QEMU exits 0). On FAIL the hart parks in a wfi loop; the
//   bench harness runs QEMU under timeout, so a FAIL is
//   observable as exit status 124.
//
// The fault and signal sites use assembler-resolved global
// labels in sccy_trap.S (never C &&label: the toolchain
// miscompiles labels-as-values at -O2, so &&label is never used
// for trap-resume addresses).

#include "../uart.h"

extern void sccy_trap_entry(void);
extern void sccy_mt_trap_entry(void);

// U-mode payload sites, defined in sccy_trap.S.
extern char sccy_u_payload_a[];
extern char sccy_u_rdcycle_a[];
extern char sccy_u_ecall_a[];
extern char sccy_u_payload_b[];
extern char sccy_u_rdcycle_b[];
extern char sccy_u_ecall_b[];

// S-mode continuations, entered via sret from the dispatcher.
void sccy_smode_test(void);
void sccy_phase_a_done(void);
void sccy_phase_b_done(void);
void sccy_unexpected(void);

// S-mode trap record, written by sccy_trap.S:
// [0] scause, [1] stval, [2] sepc at entry, [3] trap count,
// [4] resume pc chosen by the dispatcher, [5] interrupted t0,
// [6] interrupted a0, [7] interrupted t1, [8] interrupted s0.
volatile unsigned long sccy_regs[10];

// Trap history: the dispatcher copies every trap's record here
// so phase A can still inspect the rdcycle trap after the ecall
// trap overwrote sccy_regs.
#define SCCY_HIST_N 8
volatile unsigned long sccy_hist[SCCY_HIST_N][10];
volatile unsigned long sccy_hist_n;
volatile unsigned long sccy_bad_cause;
volatile unsigned long sccy_bad_sig;

// Site addresses, captured in S-mode before the drops.
volatile unsigned long sccy_site_rdcycle_a;
volatile unsigned long sccy_site_ecall_a;
volatile unsigned long sccy_site_rdcycle_b;
volatile unsigned long sccy_site_ecall_b;

#define SCCY_SIG_A 0x8a8aUL
#define SCCY_SIG_B 0x8b8bUL

#define SENTINEL 0xDEADBEEFDEADBEEFUL
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

// C dispatcher, called from sccy_trap.S with r = sccy_regs.
// Appends the trap record to the history and returns the resume
// pc: sepc+4 for the gated rdcycle (illegal instruction), or an
// S-mode continuation for the U-mode ecall signals (setting
// sstatus.SPP=1 first so sret resumes in S-mode).
unsigned long sccy_handle(volatile unsigned long *r) {
    unsigned long cause = r[0];
    unsigned long resume;
    int i;

    if (sccy_hist_n < SCCY_HIST_N) {
        for (i = 0; i < 9; i++)
            sccy_hist[sccy_hist_n][i] = r[i];
        sccy_hist_n++;
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
        if (sig == SCCY_SIG_A)
            resume = (unsigned long)sccy_phase_a_done;
        else if (sig == SCCY_SIG_B)
            resume = (unsigned long)sccy_phase_b_done;
        else {
            sccy_bad_cause = cause;
            sccy_bad_sig = sig;
            resume = (unsigned long)sccy_unexpected;
        }
        r[4] = resume;
        return resume;
    }
    sccy_bad_cause = cause;
    sccy_bad_sig = r[6];
    __asm__ volatile("li t0, 0x100\n\t"
                     "csrs sstatus, t0"
                     ::: "t0", "memory");
    resume = (unsigned long)sccy_unexpected;
    r[4] = resume;
    return resume;
}

static void print_trap(int idx, const char *tag) {
    volatile unsigned long *h = sccy_hist[idx];
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
void sccy_phase_a_done(void) {
    unsigned long rb;
    int ok;

    uart_puts("\n--- back in S-mode: phase A complete ---\n");
    uart_puts("trap history:\n");
    print_trap(0, "gated rdcycle");
    print_trap(1, "SIG_A ecall ");

    ok = (sccy_hist_n == 2);
    check(ok, "phase A: trap count is not 2");
    cks_feed(ok);
    ok = (sccy_hist[0][0] == 2);
    check(ok, "phase A: trap 1 scause is not 2 (illegal instruction)");
    cks_feed(sccy_hist[0][0]);
    ok = (sccy_hist[0][2] == sccy_site_rdcycle_a);
    check(ok, "phase A: trap 1 sepc is not at the rdcycle site");
    cks_feed(ok);
    ok = (sccy_hist[0][5] == SENTINEL);
    check(ok, "phase A: rdcycle destination changed (it retired?)");
    cks_feed(ok);
    ok = (sccy_hist[1][0] == 8);
    check(ok, "phase A: trap 2 scause is not 8 (ecall from U-mode)");
    cks_feed(sccy_hist[1][0]);
    ok = (sccy_hist[1][2] == sccy_site_ecall_a);
    check(ok, "phase A: trap 2 sepc is not at the payload ecall site");
    cks_feed(ok);
    ok = (sccy_hist[1][6] == SCCY_SIG_A);
    check(ok, "phase A: trap 2 signal is not SIG_A");
    cks_feed(ok);

    // Phase B: S-mode sets scounteren.CY itself (it is an
    // S-mode CSR); no M-mode helper needed.
    uart_puts("\nphase B: setting scounteren.CY in S-mode...\n");
    __asm__ volatile("csrs scounteren, %0" ::"r"(SCOUNTEREN_CY) : "memory");
    __asm__ volatile("csrr %0, scounteren" : "=r"(rb));
    uart_puts("  scounteren readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    ok = ((rb & SCOUNTEREN_CY) != 0);
    check(ok, "phase B: scounteren.CY did not stick");
    cks_feed(ok);

    uart_puts("dropping to U-mode with scounteren.CY=1...\n");
    drop_to_umode((void (*)(void))sccy_u_payload_b);
}

// Phase B continuation, entered in S-mode via sret from the
// dispatcher after the payload's SIG_B ecall. The two rdcycle
// samples arrive in the interrupted t0/t1 of trap 3.
void sccy_phase_b_done(void) {
    unsigned long s0, s1;
    int ok;

    uart_puts("\n--- back in S-mode: phase B complete ---\n");
    print_trap(2, "SIG_B ecall ");

    ok = (sccy_hist_n == 3);
    check(ok, "phase B: trap count is not 3");
    cks_feed(ok);
    ok = (sccy_hist[2][0] == 8);
    check(ok, "phase B: trap 3 scause is not 8 (ecall from U-mode)");
    cks_feed(sccy_hist[2][0]);
    ok = (sccy_hist[2][2] == sccy_site_ecall_b);
    check(ok, "phase B: trap 3 sepc is not at the payload ecall site");
    cks_feed(ok);
    ok = (sccy_hist[2][6] == SCCY_SIG_B);
    check(ok, "phase B: trap 3 signal is not SIG_B");
    cks_feed(ok);

    s0 = sccy_hist[2][5];
    s1 = sccy_hist[2][7];
    uart_puts("  rdcycle sample 0 (t0) = ");
    uart_put_hex(s0);
    uart_puts("\n  rdcycle sample 1 (t1) = ");
    uart_put_hex(s1);
    uart_puts("  (delta = ");
    uart_put_dec(s1 - s0);
    uart_puts(")\n");

    ok = (s1 > s0);
    check(ok, "phase B: rdcycle samples did not strictly increase");
    cks_feed(ok);
    ok = (s0 != 0);
    check(ok, "phase B: rdcycle sample 0 is zero (counter not running?)");
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

// Unexpected-trap continuation: any trap the dispatcher did not
// recognize (wrong cause, wrong signal, history overflow) lands
// here in S-mode. Report what arrived and park; the harness
// observes the FAIL as a timeout.
void sccy_unexpected(void) {
    unsigned long i;

    uart_puts("\n--- UNEXPECTED TRAP ---\n");
    uart_puts("  cause=");
    uart_put_hex(sccy_bad_cause);
    uart_puts(" signal(a0)=");
    uart_put_hex(sccy_bad_sig);
    uart_puts("\n  history depth=");
    uart_put_dec(sccy_hist_n);
    uart_puts("\n");
    for (i = 0; i < sccy_hist_n && i < SCCY_HIST_N; i++)
        print_trap((int)i, "hist");
    uart_puts("RESULT: FAIL\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

// S-mode payload driver, entered via mret with MPP=01.
void sccy_smode_test(void) {
    uart_puts("in S-mode; scounteren-cy-gate experiment begins\n");
    sccy_site_rdcycle_a = (unsigned long)sccy_u_rdcycle_a;
    sccy_site_ecall_a = (unsigned long)sccy_u_ecall_a;
    sccy_site_rdcycle_b = (unsigned long)sccy_u_rdcycle_b;
    sccy_site_ecall_b = (unsigned long)sccy_u_ecall_b;
    uart_puts("phase A payload: rdcycle site ");
    uart_put_hex(sccy_site_rdcycle_a);
    uart_puts(", ecall site ");
    uart_put_hex(sccy_site_ecall_a);
    uart_puts("\n");
    uart_puts("dropping to U-mode with scounteren.CY=0...\n");
    drop_to_umode((void (*)(void))sccy_u_payload_a);
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
    unsigned long boot_sc, rb, deleg;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("scounteren.CY gates U-mode rdcycle\n");
    uart_puts("CY clear must trap, CY set must read\n");
    uart_puts("========================================\n\n");

    // 1. Boot scounteren and the writability probe: write 0x7
    // (CY|TM|IR), read back; the readback must be exactly 0x7
    // so the later 0 write is proven to take effect rather
    // than be silently ignored.
    boot_sc = read_scounteren();
    uart_puts("boot: scounteren=");
    uart_put_hex(boot_sc);
    uart_puts("\n");

    write_scounteren(0x7UL);
    rb = read_scounteren();
    uart_puts("probe: csrw scounteren, 0x7; readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == 0x7UL, "scounteren write of 0x7 did not read back as 0x7");
    cks_feed(rb);

    // 2. Gate: write scounteren = 0, read back.
    write_scounteren(0);
    rb = read_scounteren();
    uart_puts("write: csrw scounteren, 0; readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == 0, "scounteren write of 0 did not read back as 0");
    cks_feed(rb);

    // 3. mcounteren.CY must be set: it also gates the cycle CSR
    // for S-mode and U-mode, and QEMU resets mcounteren to 0.
    // Without this, phase B would trap regardless of
    // scounteren.CY and prove nothing.
    __asm__ volatile("csrw mcounteren, %0" ::"r"(SCOUNTEREN_CY) : "memory");
    __asm__ volatile("csrr %0, mcounteren" : "=r"(rb));
    uart_puts("write: mcounteren.CY=1 (unblock the M-level gate); readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check((rb & SCOUNTEREN_CY) != 0, "mcounteren.CY did not stick");
    cks_feed((rb & SCOUNTEREN_CY) != 0);

    // 4. Trap vectors, delegation, PMP.
    __asm__ volatile("la t0, sccy_mt_trap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, sccy_trap_entry\n\t"
                     "csrw stvec, t0"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, sccy_regs\n\t"
                     "csrw sscratch, t0"
                     :
                     :
                     : "t0", "memory");
    // Delegate the illegal-instruction trap (bit 2) and the
    // U-mode ecall (bit 8) to S-mode; all other traps stay in
    // M-mode and park there.
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

    uart_puts("setup complete; dropping to S-mode...\n");

    // 5. mret with MPP=01 (S-mode) into sccy_smode_test. The
    // whole experiment runs from there.
    __asm__ volatile("la t0, sccy_smode_test\n\t"
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
