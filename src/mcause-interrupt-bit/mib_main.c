// mib_main.c: mcause bit 63, exception vs interrupt (retry of the
// 2026-09-10 item that was blocked on OpenSBI software-interrupt
// pending-bit ambiguity; this retry uses the machine timer interrupt,
// already proven on this QEMU setup, bare metal, no OpenSBI).
//
// Claim under test: mcause bit 63 is 1 for an asynchronous interrupt
// and 0 for a synchronous exception. Two deliberate traps, M-mode
// throughout:
//   Phase 1: an M-mode ecall. The handler records mcause (expect
//     0xb) and resumes past the 4-byte ecall. Published: mcause and
//     (mcause >> 63), expect 0.
//   Phase 2: the CLINT machine timer interrupt, armed via mtimecmp.
//     The handler records mcause (expect 0x8000000000000007),
//     disarms mtimecmp to all-ones, and resumes into the spin loop.
//     Published: mcause and (mcause >> 63), expect 1. A quiet window
//     after the disarm proves no re-delivery.
//
// Every expectation is an in-program check: `checks` counts the
// checks run, `fails` counts the ones that failed. The exit code and
// the RESULT line follow `fails`. Exit: on PASS the module prints
// done and parks in wfi (the bench harness runs QEMU under
// `timeout`, as with the mtvec-vectored module).

#include "../uart.h"

#define MCAUSE_ECALL_M  11UL
#define MCAUSE_MTI      0x8000000000000007UL

#define CLINT_MTIMECMP0 0x02004000UL
#define CLINT_MTIME    0x0200bff8UL  // 10 MHz timebase

#define MSTATUS_MIE (1UL << 3)
#define MIE_MTIE    (1UL << 7)

// Timer arming: 50000 mtime ticks = 5 ms. If the host deschedules the
// vCPU inside the arm window, mtime is already past mtimecmp when the
// store lands and QEMU's CLINT never fires for a past-deadline write;
// re-arm until the deadline is genuinely in the future at store time,
// up to 10 attempts (same construction as the mtvec-vectored module).
#define TIMER_AHEAD_TICKS 50000UL
#define ARM_ATTEMPTS 10

// mib_regs layout, shared with mib_trap.S:
// [0] t0 save, [1] t1 save, [2] mcause, [3] mepc, [4] ra, [5] a0.
static volatile unsigned long mib_regs[8];

static volatile unsigned long ecall_count = 0;
static volatile unsigned long ecall_mcause = 0;
static volatile unsigned long ecall_mepc = 0;
static volatile unsigned long timer_count = 0;
static volatile unsigned long timer_mcause = 0;
static volatile unsigned long timer_mepc = 0;
static volatile unsigned long unexpected_count = 0;
static volatile unsigned long unexpected_mcause = 0;

extern char mib_ecall_site;  // link-time address of the deliberate ecall
extern void mib_trap_entry(void);

static unsigned long clint_get_mtime(void) {
    return *(volatile unsigned long *)CLINT_MTIME;
}

static void clint_set_mtimecmp(unsigned long v) {
    *(volatile unsigned long *)CLINT_MTIMECMP0 = v;
}

static unsigned long clint_get_mtimecmp(void) {
    return *(volatile unsigned long *)CLINT_MTIMECMP0;
}

// Called from the asm trap entry. Records the cause per trap class:
// the deliberate ecall resumes past the 4-byte ecall, the machine
// timer interrupt disarms its own source and resumes where it fired
// (into the wait loop), anything else is counted as unexpected.
void mib_c_handle(void) {
    unsigned long cause = mib_regs[2];
    if (cause == MCAUSE_ECALL_M) {
        unsigned long epc;
        ecall_count++;
        ecall_mcause = cause;
        ecall_mepc = mib_regs[3];
        __asm__ volatile("csrr %0, mepc" : "=r"(epc));
        epc += 4;  // ecall is always a 4-byte instruction
        __asm__ volatile("csrw mepc, %0" : : "r"(epc));
    } else if (cause == MCAUSE_MTI) {
        timer_count++;
        timer_mcause = cause;
        timer_mepc = mib_regs[3];
        clint_set_mtimecmp(~0UL);  // disarm: no re-fire after mret
        // mepc untouched: resume into the spin loop.
    } else {
        unexpected_count++;
        unexpected_mcause = cause;
        // mepc untouched: resume where the trap fired.
    }
}

static unsigned long checks = 0;
static unsigned long fails = 0;

static void check(int cond, const char *msg) {
    checks++;
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

// FNV-1a 64-bit over the normalized result record: the recorded
// values only, no host-timing-dependent fields, so the checksum is
// identical on every clean run.
static unsigned long result_checksum(void) {
    unsigned long h = 1469598103934665603UL;
    unsigned long vals[9];
    int i, j;
    vals[0] = checks;
    vals[1] = fails;
    vals[2] = ecall_mcause;
    vals[3] = ecall_mcause >> 63;
    vals[4] = timer_mcause;
    vals[5] = timer_mcause >> 63;
    vals[6] = ecall_count;
    vals[7] = timer_count;
    vals[8] = unexpected_count;
    for (i = 0; i < 9; i++)
        for (j = 0; j < 8; j++) {
            h ^= (unsigned char)(vals[i] >> (8 * j));
            h *= 1099511628211UL;
        }
    return h;
}

int main(void) {
    unsigned long mtvec, timed_out, armed, a;

    uart_init();
    uart_puts("mcause-interrupt-bit: mcause bit 63, exception vs interrupt\n");

    // Disarm the timer source before enabling anything: on reset
    // mtimecmp reads 0, already below mtime, and would assert the
    // timer interrupt the moment MTIE/MIE are set.
    clint_set_mtimecmp(~0UL);

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the scratch area. Read mtvec back to confirm the install.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mib_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mib_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    uart_puts("trap: mtvec=");
    uart_put_hex(mtvec);
    uart_puts("\n");
    check((mtvec & ~3UL) == (unsigned long)mib_trap_entry,
          "mtvec did not take the handler address");
    check((mtvec & 3UL) == 0, "mtvec not in direct mode");

    // Global MIE on; the timer interrupt stays masked in mie until
    // phase 2, so only synchronous traps can fire from here on.
    __asm__ volatile("csrs mstatus, %0" : : "r"(MSTATUS_MIE));

    // Control: with no source armed, a quiet window must produce zero
    // traps, proving the traps below come from their own triggers.
    (void)wait_for_count(&ecall_count, 1, 1000000UL);
    uart_puts("control: traps-before-ecall=");
    uart_put_dec(ecall_count + timer_count + unexpected_count);
    uart_puts(" (expect 0)\n");
    check(ecall_count + timer_count + unexpected_count == 0,
          "spurious trap before any trigger");

    // Phase 1: a deliberate M-mode ecall. Synchronous trap, so bit 63
    // of mcause must read 0 and the cause must be 11. The ecall site
    // carries a global label so the handler-recorded mepc can be
    // checked against the exact instruction address.
    __asm__ volatile(
        ".global mib_ecall_site\n"
        "mib_ecall_site:\n"
        "ecall\n"
        :
        :
        : "memory");
    timed_out = wait_for_count(&ecall_count, 1, 10000000UL);
    uart_puts("ecall: count=");
    uart_put_dec(ecall_count);
    uart_puts(" mcause=");
    uart_put_hex(ecall_mcause);
    uart_puts(" bit63=");
    uart_put_dec(ecall_mcause >> 63);
    uart_puts(" mepc=");
    uart_put_hex(ecall_mepc);
    uart_puts(" (expect mcause=0xb bit63=0)\n");
    check(!timed_out, "M-mode ecall did not trap");
    check(ecall_count == 1, "ecall trap did not fire exactly once");
    check(ecall_mcause == MCAUSE_ECALL_M, "ecall mcause != 0xb");
    check((ecall_mcause >> 63) == 0, "ecall mcause bit 63 set");
    check(ecall_mepc == (unsigned long)&mib_ecall_site,
          "ecall mepc != the ecall instruction address");

    // Phase 2: the machine timer interrupt. Asynchronous trap, so bit
    // 63 of mcause must read 1 and the cause must be 7. Arm with the
    // bounded retry described above, then spin until delivery.
    __asm__ volatile("csrs mie, %0" : : "r"(MIE_MTIE));
    armed = 0;
    for (a = 0; a < ARM_ATTEMPTS && !armed; a++) {
        unsigned long t = clint_get_mtime();
        clint_set_mtimecmp(t + TIMER_AHEAD_TICKS);
        if (clint_get_mtime() < clint_get_mtimecmp())
            armed = 1;
    }
    uart_puts("timer: armed=");
    uart_put_dec(armed);
    uart_puts(" (mtimecmp set 50000 ticks ahead, up to 10 arm attempts)\n");
    check(armed, "could not arm mtimecmp in the future");
    timed_out = wait_for_count(&timer_count, 1, 100000000UL);
    uart_puts("timer: count=");
    uart_put_dec(timer_count);
    uart_puts(" mcause=");
    uart_put_hex(timer_mcause);
    uart_puts(" bit63=");
    uart_put_dec(timer_mcause >> 63);
    uart_puts(" (expect mcause=0x8000000000000007 bit63=1)\n");
    check(!timed_out, "machine timer interrupt never delivered");
    check(timer_count == 1, "timer trap did not fire exactly once");
    check(timer_mcause == MCAUSE_MTI,
          "timer mcause != 0x8000000000000007");
    check((timer_mcause >> 63) == 1, "timer mcause bit 63 clear");

    // The handler disarmed the source: mtimecmp must read all-ones,
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

    // No trap outside the two deliberate ones may have fired.
    uart_puts("unexpected: count=");
    uart_put_dec(unexpected_count);
    uart_puts(" (expect 0)\n");
    check(unexpected_count == 0, "unexpected trap fired");
    if (unexpected_count) {
        uart_puts("unexpected: mcause=");
        uart_put_hex(unexpected_mcause);
        uart_puts("\n");
    }

    uart_puts("checks=");
    uart_put_dec(checks);
    uart_puts(" mismatches=");
    uart_put_dec(fails);
    uart_puts(" checksum=");
    uart_put_hex(result_checksum());
    uart_puts("\n");
    uart_puts("RESULT: ");
    uart_puts(fails == 0 ? "PASS" : "FAIL");
    uart_puts("\n");

    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
