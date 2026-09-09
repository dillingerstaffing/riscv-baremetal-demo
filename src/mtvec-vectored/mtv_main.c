// mtv_main.c: mtvec vectored dispatch check (backlog item 74).
//
// Mechanism under test: with mtvec MODE=1 (vectored), trap delivery.
// Measured on QEMU 8.2.2 (see PROOF.md): asynchronous interrupts
// (e.g. the machine timer, code 7) vector to BASE + 4*C, while
// synchronous exceptions (e.g. a U-mode ecall, code 8) enter at BASE.
// The module asserts exactly this observed behavior; it does not
// assume the spec's vectored rule for synchronous traps.
//
// Two traps are exercised:
//   1. A synchronous ecall from U-mode (exception code 8; an M-mode
//      ecall would raise code 11, so the test drops to U-mode for two
//      consecutive ecalls). Measured result on QEMU 8.2.2: the trap
//      goes to BASE (entry 0), because this QEMU only vectors
//      asynchronous traps (see PROOF.md). mcause reads 8 and the
//      recorded landing address equals BASE. The first trap resumes
//      in U-mode at the second ecall; the second returns to M-mode.
//   2. A machine timer interrupt (exception code 7), armed via the
//      CLINT mtimecmp. The stub for entry 7 must run, mcause must read
//      0x8000000000000007, and the recorded landing address must equal
//      BASE + 28.
//
// The landing address is recorded by the stub that actually ran, from
// the link-time address of its own table entry, so a mis-vectored trap
// records the wrong entry and fails the check. BASE itself is read back
// from mtvec and cross-checked against the mtv_table symbol.

#include "../uart.h"

// QEMU virt CLINT: mtimecmp for hart 0 and the 64-bit mtime.
#define CLINT_MTIMECMP0 0x02004000UL
#define CLINT_MTIME    0x0200bff8UL  // 10 MHz timebase

#define MSTATUS_MIE (1UL << 3)
#define MIE_MTIE    (1UL << 7)

// mcause encodings under test: interrupt bit plus code, or code alone.
// Note: an ecall executed in M-mode raises code 11, so the code-8
// synchronous trap is produced by dropping to U-mode first (the
// payload in mtv_trap.S); only a U-mode ecall yields mcause == 8.
#define MCAUSE_ECALL_U  8UL
#define MCAUSE_MTI      0x8000000000000007UL

// mtv_regs layout, shared with mtv_trap.S:
// [0] landing entry address, [1] stub cause index, [2] mcause,
// [3] mepc at entry, [4] entry cycle stamp, [5] ra, [6] t1,
// [7..11] t2..t6, [12..19] a0..a7, [20] resume mepc (C writes it).
static volatile unsigned long mtv_regs[24];

static volatile unsigned long ecall_count = 0;
static volatile unsigned long timer_count = 0;
static volatile unsigned long unexpected_count = 0;
static volatile unsigned long unexpected_mcause = 0;

// Set by the C handler after the second U-mode ecall; the asm common
// handler then switches mstatus.MPP back to M and resumes at
// mtv_m_resume instead of the U-mode payload.
volatile unsigned long mtv_return_m = 0;

static volatile unsigned long ecall_landing = 0;
static volatile unsigned long ecall_mcause = 0;
static volatile unsigned long ecall2_landing = 0;
static volatile unsigned long ecall2_mcause = 0;
static volatile unsigned long timer_landing = 0;
static volatile unsigned long timer_mcause = 0;

extern char mtv_table;   // vector table base (link-time address)
extern char mtv_vec7;    // entry for exception code 7
extern char mtv_vec8;    // entry for exception code 8
extern void mtv_umode_entry(void);  // mret into the U-mode ecall payload

static unsigned long clint_get_mtime(void) {
    return *(volatile unsigned long *)CLINT_MTIME;
}

static void clint_set_mtimecmp(unsigned long v) {
    *(volatile unsigned long *)CLINT_MTIMECMP0 = v;
}

static unsigned long clint_get_mtimecmp(void) {
    return *(volatile unsigned long *)CLINT_MTIMECMP0;
}

// Called from the asm common handler. Decides the resume address:
// the first U-mode ecall resumes at the second ecall (still U-mode),
// the second asks the asm handler to return to M-mode, the timer
// interrupt resumes where it fired after disarming the source.
void mtv_c_handle(void) {
    unsigned long landing = mtv_regs[0];
    unsigned long idx = mtv_regs[1];
    unsigned long mcause = mtv_regs[2];
    unsigned long mepc = mtv_regs[3];

    if (mcause == MCAUSE_ECALL_U) {
        ecall_count++;
        if (ecall_count == 1) {
            ecall_landing = landing;
            ecall_mcause = mcause;
            mtv_regs[20] = mepc + 4;  // second ecall, still in U-mode
        } else if (ecall_count == 2) {
            ecall2_landing = landing;
            ecall2_mcause = mcause;
            mtv_return_m = 1;  // asm: MPP=M, resume at mtv_m_resume
            mtv_regs[20] = mepc + 4;
        } else {
            unexpected_count++;
            unexpected_mcause = mcause;
            mtv_regs[20] = mepc;
        }
    } else if (mcause == MCAUSE_MTI) {
        timer_count++;
        timer_landing = landing;
        timer_mcause = mcause;
        clint_set_mtimecmp(~0UL);  // disarm: no re-fire after mret
        mtv_regs[20] = mepc;
    } else {
        unexpected_count++;
        unexpected_mcause = mcause;
        mtv_regs[20] = mepc;
    }
    (void)idx;
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

// Poll with a spin budget until *counter reaches want, so a missing
// trap is a FAIL, not a hang. Returns 1 on timeout.
static int wait_for_count(volatile unsigned long *counter,
                          unsigned long want, unsigned long budget) {
    unsigned long spins = 0;
    while (*counter < want && spins < budget)
        spins++;
    return *counter < want;
}

int main(void) {
    unsigned long base, mtvec, mode;
    unsigned long expect7, expect8;
    int timed_out;

    uart_init();
    uart_puts("mtvec-vectored: vectored trap dispatch test\n");

    // Disarm the timer source before enabling anything: on reset
    // mtimecmp reads 0, which is already below mtime and would assert
    // the timer interrupt the moment MTIE/MIE are set.
    clint_set_mtimecmp(~0UL);

    // Install vectored mode: BASE | MODE=1. Read mtvec back and check
    // both fields against the link-time table address.
    __asm__ volatile("csrw mtvec, %0" :
                     : "r"((unsigned long)&mtv_table | 1UL));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mtv_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    base = mtvec & ~3UL;
    mode = mtvec & 3UL;
    uart_puts("mtvec: written=");
    uart_put_hex((unsigned long)&mtv_table | 1UL);
    uart_puts(" readback=");
    uart_put_hex(mtvec);
    uart_puts("\n");
    check(mode == 1, "mtvec MODE != 1 (vectored)");
    check(base == (unsigned long)&mtv_table,
          "mtvec BASE != &mtv_table");
    check((base & 0xFFUL) == 0, "vector table not 256-byte aligned");

    // The table layout the hardware indexes: entry N at BASE + 4*N.
    // Cross-check against the actual entry symbols.
    expect7 = base + 4 * 7;
    expect8 = base + 4 * 8;
    uart_puts("table: base=");
    uart_put_hex(base);
    uart_puts(" entry7=");
    uart_put_hex((unsigned long)&mtv_vec7);
    uart_puts(" (expect ");
    uart_put_hex(expect7);
    uart_puts(") entry8=");
    uart_put_hex((unsigned long)&mtv_vec8);
    uart_puts(" (expect ");
    uart_put_hex(expect8);
    uart_puts(")\n");
    check((unsigned long)&mtv_vec7 == expect7, "entry7 != BASE+28");
    check((unsigned long)&mtv_vec8 == expect8, "entry8 != BASE+32");

    // Global MIE on, timer interrupt still masked: only synchronous
    // traps can fire from here on.
    __asm__ volatile("csrs mstatus, %0" : : "r"(MSTATUS_MIE));

    // Control: with no source armed, a quiet window must produce zero
    // traps, proving the traps below come from their own triggers.
    (void)wait_for_count(&ecall_count, 1, 1000000UL);
    uart_puts("control: traps-before-ecall=");
    uart_put_dec(ecall_count + timer_count + unexpected_count);
    uart_puts(" (expect 0)\n");
    check(ecall_count + timer_count + unexpected_count == 0,
          "spurious trap before any trigger");

    // PMP: with no PMP entry programmed, U-mode has access to no
    // address at all (M-mode keeps full access). Open the whole
    // address space with one unlocked NAPOT R/W/X entry before the
    // drop; without this the first U-mode instruction fetch raises
    // an instruction access fault. (Same construction as src/umode/.)
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t"  // A=NAPOT, R/W/X, unlocked
                     "csrw pmpcfg0, t0\n\t"
                     :
                     :
                     : "t0", "memory");

    // Trap 1+2: drop to U-mode and execute two ecalls there. An
    // ecall in U-mode raises exception code 8; the first trap resumes
    // in U-mode at the second ecall, the second returns to M-mode.
    // mtv_umode_entry does not return normally: control comes back
    // via mtv_m_resume after the second trap.
    uart_puts("ecall: entering U-mode for two ecalls...\n");
    mtv_umode_entry();
    uart_puts("ecall: back in M-mode\n");
    uart_puts("ecall: count=");
    uart_put_dec(ecall_count);
    uart_puts(" mcause1=");
    uart_put_hex(ecall_mcause);
    uart_puts(" landing1=");
    uart_put_hex(ecall_landing);
    uart_puts(" mcause2=");
    uart_put_hex(ecall2_mcause);
    uart_puts(" landing2=");
    uart_put_hex(ecall2_landing);
    uart_puts("\n");
    // Measured QEMU 8.2.2 behavior: synchronous exceptions are NOT
    // vectored (see the timer-phase comment and PROOF.md). The
    // U-mode ecall (code 8) traps to BASE, i.e. entry 0's stub runs,
    // while mcause still reads 8. The checks below assert exactly
    // this observed behavior: the stub index must be 0 and the
    // landing address must equal BASE.
    uart_puts("ecall: note: sync trap landed at BASE (entry 0), not BASE+32\n");
    check(ecall_count == 2, "expected exactly two U-mode ecall traps");
    check(ecall_mcause == MCAUSE_ECALL_U, "ecall1 mcause != 8");
    check(ecall_landing == base, "ecall1 landing != BASE");
    check(ecall2_mcause == MCAUSE_ECALL_U, "ecall2 mcause != 8");
    check(ecall2_landing == base, "ecall2 landing != BASE");
    check(timer_count == 0, "timer fired during ecall phase");
    check(unexpected_count == 0, "unexpected trap during ecall phase");

    // Trap 2: machine timer interrupt, exception code 7. This is an
    // asynchronous trap, so vectored mode applies on this QEMU (see
    // PROOF.md): it must land at BASE + 28.
    //
    // Arm mtimecmp with a bounded retry. If the host deschedules the
    // vCPU inside the arm window, mtime is already past mtimecmp when
    // the store lands, and QEMU's CLINT never fires for a
    // past-deadline write (the same hazard the mtimecmp module's
    // retry loop handles). Re-arm until the deadline is genuinely in
    // the future at store time.
    __asm__ volatile("csrs mie, %0" : : "r"(MIE_MTIE));
    {
        int armed = 0;
        int a;
        for (a = 0; a < 10 && !armed; a++) {
            unsigned long t = clint_get_mtime();
            clint_set_mtimecmp(t + 50000UL);  // 5 ms of mtime
            if (clint_get_mtime() < clint_get_mtimecmp())
                armed = 1;
        }
        uart_puts("timer: armed-after-retries=");
        uart_put_dec((unsigned long)(armed ? 1 : 0));
        uart_puts("\n");
        check(armed, "could not arm mtimecmp in the future");
    }
    uart_puts("timer: waiting...\n");
    timed_out = wait_for_count(&timer_count, 1, 100000000UL);
    uart_puts("timer: count=");
    uart_put_dec(timer_count);
    uart_puts(" mcause=");
    uart_put_hex(timer_mcause);
    uart_puts(" landing=");
    uart_put_hex(timer_landing);
    uart_puts(" (expect ");
    uart_put_hex(expect7);
    uart_puts(")\n");
    check(!timed_out, "machine timer interrupt never delivered");
    check(timer_count == 1, "timer trap did not fire exactly once");
    check(timer_mcause == MCAUSE_MTI,
          "timer mcause != 0x8000000000000007");
    check(timer_landing == expect7, "timer landed != BASE+28");
    check(unexpected_count == 0, "unexpected trap during timer phase");

    // The handler disarmed the source: mtimecmp must read all ones,
    // and no second delivery may arrive in a further quiet window.
    uart_puts("timer: mtimecmp-after-handler=");
    uart_put_hex(clint_get_mtimecmp());
    uart_puts(" (expect 0xffffffffffffffff)\n");
    check(clint_get_mtimecmp() == ~0UL,
          "mtimecmp not disarmed by the handler");
    (void)wait_for_count(&timer_count, 2, 2000000UL);
    uart_puts("timer: count-after-quiet=");
    uart_put_dec(timer_count);
    uart_puts(" (expect 1)\n");
    check(timer_count == 1, "timer re-fired after disarm");

    __asm__ volatile("csrc mie, %0" : : "r"(MIE_MTIE));

    if (fails == 0) {
        uart_puts("RESULT: PASS (2x U-mode ecall->BASE mcause=8 [sync not vectored on QEMU], timer->BASE+28 mcause=0x8000000000000007)\n");
    } else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
