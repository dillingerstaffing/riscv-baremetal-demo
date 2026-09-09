// ecall_main.c: ecall ABI round-trip between an S-mode payload and an
// M-mode trap handler.
//
// One mechanism: the RISC-V ecall instruction traps to M-mode, and the
// handler is responsible for the calling convention across the
// privilege boundary. This program drops to S-mode, loads three sets of
// known constants into a0-a7 and t0-t6, issues an ecall with a service
// number in a7, and checks that the M-mode handler returns a computed
// result in a0 while every other register comes back bit-identical.
// The handler independently logs what it received, so the report shows
// both sides of the round trip: what the payload loaded, what the
// handler saw, and what came back.
//
// Service 1: return a0 = a0^a1^a2^a3^a4^a5, all other registers intact.
// Service 0xE11: end of test, resume M-mode at m_report to print results.

#include "../uart.h"

extern void ecall_trap_entry(void);

// Trap save area: ecall_regs[r] holds register xr on trap entry.
// mscratch points here while the test runs.
volatile unsigned long ecall_regs[32];

// Per-call log, written by the M-mode handler. Each row:
// [0] mcause, [1] mepc, [2] instruction word at mepc,
// [3..10] a0..a7 as received, [11..17] t0..t6 as received,
// [18] result placed in a0, [19] resume pc.
#define ELOG_N 4
volatile unsigned long ecall_log[ELOG_N][20];
volatile unsigned long ecall_ncalls;
volatile unsigned long ecall_unexpected;
volatile unsigned long exit_resume_pc;

#define SVC_XOR_ARGS 1ul
#define SVC_EXIT_MMODE 0xE11ul

#define NSETS 3

static const unsigned long set_args[NSETS][6] = {
    {0x0000000000000001ul, 0x0000000000000002ul, 0x0000000000000003ul,
     0x0000000000000004ul, 0x0000000000000005ul, 0x0000000000000006ul},
    {0x0000000000000000ul, 0xFFFFFFFFFFFFFFFFul, 0xAAAAAAAAAAAAAAAAul,
     0x5555555555555555ul, 0x8000000000000000ul, 0x0000000000000001ul},
    {0x0123456789ABCDEFul, 0xFEDCBA9876543210ul, 0xAAAAAAAAAAAAAAAAul,
     0x5555555555555555ul, 0xDEADBEEFCAFEBABEul, 0x0F0F0F0F0F0F0F0Ful},
};
static const unsigned long set_a6[NSETS] = {
    0xA6A6A6A6A6A6A6A6ul, 0x6A6A6A6A6A6A6A6Aul, 0x1234567890ABCDEFul,
};
static const unsigned long set_t[NSETS][7] = {
    {0x1111111111111111ul, 0x2222222222222222ul, 0x3333333333333333ul,
     0x4444444444444444ul, 0x5555555555555555ul, 0x6666666666666666ul,
     0x7777777777777777ul},
    {0x8888888888888888ul, 0x9999999999999999ul, 0xAAAAAAAAAAAAAAAAul,
     0xBBBBBBBBBBBBBBBBul, 0xCCCCCCCCCCCCCCCCul, 0xDDDDDDDDDDDDDDDDul,
     0xEEEEEEEEEEEEEEEEul},
    {0xFFFFFFFFFFFFFFFFul, 0x0000000000000000ul, 0xAAAAAAAAAAAAAAAAul,
     0x5555555555555555ul, 0x0F0F0F0F0F0F0F0Ful, 0xF0F0F0F0F0F0F0F0ul,
     0x8000000000000000ul},
};

// Payload-side record, filled in S-mode: out[s][0..7] = a0..a7 after
// the ecall, out[s][8..14] = t0..t6 after the ecall; ok[s] = 1 when
// every check passed.
volatile unsigned long payload_out[NSETS][15];
volatile unsigned long payload_expected[NSETS];
volatile unsigned long payload_ok[NSETS];

static unsigned long csr_read_mcause(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mcause" : "=r"(v));
    return v;
}

static unsigned long csr_read_mepc(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mepc" : "=r"(v));
    return v;
}

// M-mode dispatcher, called from ecall_trap.S with r = ecall_regs.
// Logs the trap, computes the ecall result, returns the resume pc.
// Runs with all interrupted registers already saved, so it can use the
// C calling convention freely; only ecall_regs[] is the shared state.
unsigned long ecall_handle(volatile unsigned long *r) {
    unsigned long mcause = csr_read_mcause();
    unsigned long mepc = csr_read_mepc();
    unsigned long insn = *(volatile unsigned int *)mepc;
    unsigned long n = ecall_ncalls;
    unsigned long resume = mepc + 4;
    unsigned long res = 0;

    if (n >= ELOG_N) {
        for (;;)
            __asm__ volatile("wfi");
    }
    ecall_log[n][0] = mcause;
    ecall_log[n][1] = mepc;
    ecall_log[n][2] = insn;
    for (int i = 0; i < 8; i++)
        ecall_log[n][3 + i] = r[10 + i];  // a0..a7
    {
        // t0-t6 are x5, x6, x7, x28, x29, x30, x31.
        static const int tno[7] = {5, 6, 7, 28, 29, 30, 31};
        for (int i = 0; i < 7; i++)
            ecall_log[n][11 + i] = r[tno[i]];
    }

    unsigned long svc = r[17];  // a7
    // Ground truth for an S-mode ecall: mcause 9, and the trapped
    // instruction word must decode as ecall (0x00000073).
    if (mcause == 9 && insn == 0x00000073ul) {
        if (svc == SVC_XOR_ARGS) {
            res = r[10] ^ r[11] ^ r[12] ^ r[13] ^ r[14] ^ r[15];
            r[10] = res;  // a0 = result; the asm restores the rest intact
        } else if (svc == SVC_EXIT_MMODE) {
            resume = exit_resume_pc;
        } else {
            ecall_unexpected = 1;
        }
    } else {
        ecall_unexpected = 1;
    }
    ecall_log[n][18] = res;
    ecall_log[n][19] = resume;
    ecall_ncalls = n + 1;
    return resume;
}

// Issue one ecall with the argument registers pinned to the given
// values. Each register variable is bound to its ABI register, so the
// asm body is a single ecall with the compiler holding every register
// steady across it; on return the values are read back into out[].
static void do_ecall(unsigned long svc,
                     const unsigned long a[6], unsigned long a6,
                     const unsigned long t[7],
                     unsigned long out[15]) {
    register unsigned long r_a0 __asm__("a0") = a[0];
    register unsigned long r_a1 __asm__("a1") = a[1];
    register unsigned long r_a2 __asm__("a2") = a[2];
    register unsigned long r_a3 __asm__("a3") = a[3];
    register unsigned long r_a4 __asm__("a4") = a[4];
    register unsigned long r_a5 __asm__("a5") = a[5];
    register unsigned long r_a6 __asm__("a6") = a6;
    register unsigned long r_a7 __asm__("a7") = svc;
    register unsigned long r_t0 __asm__("t0") = t[0];
    register unsigned long r_t1 __asm__("t1") = t[1];
    register unsigned long r_t2 __asm__("t2") = t[2];
    register unsigned long r_t3 __asm__("t3") = t[3];
    register unsigned long r_t4 __asm__("t4") = t[4];
    register unsigned long r_t5 __asm__("t5") = t[5];
    register unsigned long r_t6 __asm__("t6") = t[6];
    __asm__ volatile("ecall"
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3),
                       "+r"(r_a4), "+r"(r_a5), "+r"(r_a6), "+r"(r_a7),
                       "+r"(r_t0), "+r"(r_t1), "+r"(r_t2), "+r"(r_t3),
                       "+r"(r_t4), "+r"(r_t5), "+r"(r_t6)
                     :
                     : "memory");
    out[0] = r_a0;
    out[1] = r_a1;
    out[2] = r_a2;
    out[3] = r_a3;
    out[4] = r_a4;
    out[5] = r_a5;
    out[6] = r_a6;
    out[7] = r_a7;
    out[8] = r_t0;
    out[9] = r_t1;
    out[10] = r_t2;
    out[11] = r_t3;
    out[12] = r_t4;
    out[13] = r_t5;
    out[14] = r_t6;
}

// Run one argument set in S-mode and verify the round trip locally.
static void run_set(int s) {
    unsigned long out[15];
    unsigned long expected = set_args[s][0] ^ set_args[s][1] ^ set_args[s][2] ^
                             set_args[s][3] ^ set_args[s][4] ^ set_args[s][5];
    payload_expected[s] = expected;
    do_ecall(SVC_XOR_ARGS, set_args[s], set_a6[s], set_t[s], out);
    int ok = 1;
    if (out[0] != expected)
        ok = 0;  // a0 must be the handler's result
    for (int i = 1; i < 6; i++)
        if (out[i] != set_args[s][i])
            ok = 0;  // a1..a5 intact
    if (out[6] != set_a6[s])
        ok = 0;  // a6 intact
    if (out[7] != SVC_XOR_ARGS)
        ok = 0;  // a7 intact
    for (int i = 0; i < 7; i++)
        if (out[8 + i] != set_t[s][i])
            ok = 0;  // t0..t6 intact
    for (int i = 0; i < 15; i++)
        payload_out[s][i] = out[i];
    payload_ok[s] = (unsigned long)ok;
}

// S-mode payload entry. Runs the three argument sets, then issues the
// exit ecall; the handler resumes M-mode at m_report, so the wfi loop
// below is unreachable in a correct run.
void s_payload(void) {
    for (int s = 0; s < NSETS; s++)
        run_set(s);
    {
        unsigned long out[15];
        static const unsigned long z[6] = {0, 0, 0, 0, 0, 0};
        static const unsigned long zt[7] = {0, 0, 0, 0, 0, 0, 0};
        do_ecall(SVC_EXIT_MMODE, z, 0, zt, out);
    }
    for (;;)
        __asm__ volatile("wfi");
}

// M-mode report, entered via mret from the exit ecall. Prints the
// handler-side log and the payload-side record, cross-checks them, and
// prints the verdict.
void m_report(void) {
    int pass = 1;
    uart_puts("\n--- ecall ABI round-trip report (M-mode) ---\n");
    if (ecall_unexpected) {
        uart_puts("unexpected trap cause, instruction, or service number\n");
        pass = 0;
    }
    if (ecall_ncalls != NSETS + 1) {
        uart_puts("wrong number of handler calls\n");
        pass = 0;
    }
    for (int n = 0; n < NSETS; n++) {
        uart_puts("call ");
        uart_put_dec((unsigned long)n);
        uart_puts(": mcause=");
        uart_put_hex(ecall_log[n][0]);
        uart_puts(" mepc=");
        uart_put_hex(ecall_log[n][1]);
        uart_puts(" insn=");
        uart_put_hex(ecall_log[n][2]);
        uart_puts(" svc(a7)=");
        uart_put_hex(ecall_log[n][3 + 7]);
        uart_puts("\n  handler received a0-a5:");
        for (int i = 0; i < 6; i++) {
            uart_puts(" ");
            uart_put_hex(ecall_log[n][3 + i]);
        }
        uart_puts("\n  handler returned a0=");
        uart_put_hex(ecall_log[n][18]);
        uart_puts(" resume=");
        uart_put_hex(ecall_log[n][19]);
        uart_puts("\n");
        // Ground-truth checks for this call.
        if (ecall_log[n][0] != 9)
            pass = 0;  // must be an S-mode ecall
        if (ecall_log[n][2] != 0x73)
            pass = 0;  // trapped instruction must decode as ecall
        for (int i = 0; i < 6; i++)
            if (ecall_log[n][3 + i] != set_args[n][i])
                pass = 0;  // handler saw exactly what the payload loaded
        if (ecall_log[n][3 + 6] != set_a6[n])
            pass = 0;  // a6 as received
        if (ecall_log[n][3 + 7] != SVC_XOR_ARGS)
            pass = 0;  // service number as received
        for (int i = 0; i < 7; i++)
            if (ecall_log[n][11 + i] != set_t[n][i])
                pass = 0;  // t0..t6 as received
        if (payload_ok[n] != 1)
            pass = 0;
        if (payload_out[n][0] != ecall_log[n][18])
            pass = 0;  // payload's a0 equals the handler's computed result
        if (payload_out[n][0] != payload_expected[n])
            pass = 0;  // and equals the independently computed XOR
    }
    uart_puts("payload view (S-mode, after each ecall):\n");
    for (int n = 0; n < NSETS; n++) {
        uart_puts("  set ");
        uart_put_dec((unsigned long)n);
        uart_puts(": a0 after=");
        uart_put_hex(payload_out[n][0]);
        uart_puts(" expected=");
        uart_put_hex(payload_expected[n]);
        uart_puts(" a1-a5,a6,a7,t0-t6 intact: ");
        uart_puts(payload_ok[n] ? "yes" : "NO");
        uart_puts("\n");
        uart_puts("    after a1-a7:");
        for (int i = 1; i < 8; i++) {
            uart_puts(" ");
            uart_put_hex(payload_out[n][i]);
        }
        uart_puts("\n    after t0-t6:");
        for (int i = 8; i < 15; i++) {
            uart_puts(" ");
            uart_put_hex(payload_out[n][i]);
        }
        uart_puts("\n");
    }
    if (pass)
        uart_puts("RESULT: PASS\n");
    else
        uart_puts("RESULT: FAIL\n");
    for (;;)
        __asm__ volatile("wfi");
}

int main(void) {
    uart_init();
    uart_puts("ecall ABI round-trip: S-mode payload, M-mode handler\n");
    __asm__ volatile("csrw mtvec, %0" ::"r"(&ecall_trap_entry));
    __asm__ volatile("csrw mscratch, %0" ::"r"(ecall_regs));
    ecall_ncalls = 0;
    ecall_unexpected = 0;
    exit_resume_pc = (unsigned long)m_report;
    // PMP: with no PMP entry programmed, S-mode has no access to any
    // address (M-mode keeps full access, lower modes default-deny).
    // Open the whole address space with one NAPOT R/W/X entry before
    // the drop; without this the first S-mode instruction fetch raises
    // an instruction access fault (observed: mcause=1 at the S-mode
    // entry while debugging this module).
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t"  // A=NAPOT, R/W/X, unlocked
                     "csrw pmpcfg0, t0\n\t"
                     :
                     :
                     : "t0", "memory");
    uart_puts("dropping to S-mode for the payload...\n");
    // Drop to S-mode at s_payload. sret resumes at sepc and takes the
    // target privilege from sstatus.SPP (this is the pattern the smode
    // module uses; mret with MPP=S is not used here). The resume
    // address is a plain symbol reference, not a C labels-as-values
    // address (the distro gcc 13.2.0 miscompiles &&label at -O2).
    __asm__ volatile("la t0, s_payload\n\t"
                     "csrw sepc, t0\n\t"
                     "csrr t0, sstatus\n\t"
                     "ori t0, t0, 0x100\n\t"  // SPP = 1 (S-mode)
                     "csrw sstatus, t0\n\t"
                     "sret\n\t"
                     :
                     :
                     : "t0", "memory");
    // Unreachable: sret lands in s_payload (S-mode); the exit ecall
    // resumes M-mode at m_report.
    for (;;)
        __asm__ volatile("wfi");
}
