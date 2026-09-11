// d0_main.c: mtvec direct-mode trap landing check (backlog item 138).
//
// Mechanism under test: with mtvec MODE=0 (direct), every trap,
// synchronous or asynchronous, enters at the single BASE address.
// The module stays in M-mode throughout, writes mtvec = BASE with
// MODE=0, reads it back, then provokes two real traps and checks
// that both land at BASE (not BASE + 4*cause):
//
//   1. A deliberate M-mode ecall: mcause must read 11 (exception
//      code 11 is the M-mode environment call; code 8 is a U-mode
//      ecall, code 9 an S-mode ecall), the recorded landing must
//      equal BASE, and the recorded mepc must equal the ecall's own
//      address, captured with an in-asm numeric local label.
//   2. A machine timer interrupt (mcause 0x8000000000000007), armed
//      via the CLINT mtimecmp: the recorded landing must equal BASE
//      as well.
//
// There is exactly one trap entry in the image, so a landing anywhere
// but BASE has nowhere else to be; the handler records the address it
// actually entered through, and the main program cross-checks every
// landing against the mtvec BASE readback. Traps with any other
// mcause are counted as unexpected.
//
// On PASS the module shuts the machine down via the virt test-device
// finisher (QEMU exits 0); on FAIL it parks the hart.

#include "../uart.h"

// QEMU virt CLINT: mtimecmp for hart 0 and the 64-bit mtime.
#define CLINT_MTIMECMP0 0x02004000UL
#define CLINT_MTIME    0x0200bff8UL  // 10 MHz timebase

// virt test device: writing 0x5555 to 0x100000 shuts the machine down.
#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when the transmitter is
// fully empty, so the final RESULT line is on the wire before the
// finisher write shuts the machine down.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

#define MSTATUS_MIE (1UL << 3)
#define MIE_MTIE    (1UL << 7)

// mcause encodings under test: an M-mode ecall raises code 11 (the
// M-mode environment call; code 9 is the S-mode ecall, which this
// M-mode-only module never issues), the machine timer interrupt is
// interrupt bit + code 7.
#define MCAUSE_ECALL_M 11UL
#define MCAUSE_MTI     0x8000000000000007UL

// d0_regs layout, shared with d0_trap.S:
// [0] landing address, [1] mcause, [2] mepc at entry, [3] ra,
// [4] t1, [5..9] t2..t6, [10..17] a0..a7, [18] resume mepc (C writes).
static volatile unsigned long d0_regs[20];

static volatile unsigned long ecall_count = 0;
static volatile unsigned long timer_count = 0;
static volatile unsigned long unexpected_count = 0;
static volatile unsigned long unexpected_mcause = 0;

static volatile unsigned long ecall_landing = 0;
static volatile unsigned long ecall_mcause = 0;
static volatile unsigned long ecall_mepc = 0;
static volatile unsigned long timer_landing = 0;
static volatile unsigned long timer_mcause = 0;

extern char d0_trap_entry;  // the single trap entry (link-time address)

static unsigned long clint_get_mtime(void) {
    return *(volatile unsigned long *)CLINT_MTIME;
}

static void clint_set_mtimecmp(unsigned long v) {
    *(volatile unsigned long *)CLINT_MTIMECMP0 = v;
}

static unsigned long clint_get_mtimecmp(void) {
    return *(volatile unsigned long *)CLINT_MTIMECMP0;
}

// Called from the asm trap handler. Decides the resume address: the
// M-mode ecall resumes past itself (mepc + 4), the timer interrupt
// resumes where it fired after disarming the source, anything else is
// recorded as unexpected.
void d0_c_handle(void) {
    unsigned long landing = d0_regs[0];
    unsigned long mcause = d0_regs[1];
    unsigned long mepc = d0_regs[2];

    if (mcause == MCAUSE_ECALL_M) {
        ecall_count++;
        ecall_landing = landing;
        ecall_mcause = mcause;
        ecall_mepc = mepc;
        d0_regs[18] = mepc + 4;
    } else if (mcause == MCAUSE_MTI) {
        timer_count++;
        timer_landing = landing;
        timer_mcause = mcause;
        clint_set_mtimecmp(~0UL);  // disarm: no re-fire after mret
        d0_regs[18] = mepc;
    } else {
        unexpected_count++;
        unexpected_mcause = mcause;
        d0_regs[18] = mepc;
    }
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

// FNV-1a 64-bit, the checksum published in the log. Hashed, in order:
// mtvec written, mtvec readback, ecall mcause, ecall landing, ecall
// mepc, timer mcause, timer landing.
static unsigned long fnv1a(unsigned long h, unsigned long v) {
    h ^= v;
    h *= 0x100000001b3UL;
    return h;
}

int main(void) {
    unsigned long base, mtvec, mode, written;
    unsigned long ecall_addr, checksum;
    int timed_out;

    uart_init();
    uart_puts("mtvec-mode0-direct: direct-mode single-entry trap test\n");

    // Disarm the timer source before enabling anything: on reset
    // mtimecmp reads 0, which is already below mtime and would assert
    // the timer interrupt the moment MTIE/MIE are set.
    clint_set_mtimecmp(~0UL);

    // Install direct mode: BASE | MODE=0. Read mtvec back and check
    // the mode bits read 0 and the base matches the trap entry.
    written = (unsigned long)&d0_trap_entry;
    __asm__ volatile("csrw mtvec, %0" : : "r"(written));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)d0_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    base = mtvec & ~3UL;
    mode = mtvec & 3UL;
    uart_puts("mtvec: written=");
    uart_put_hex(written);
    uart_puts(" readback=");
    uart_put_hex(mtvec);
    uart_puts("\n");
    check(mtvec == written, "mtvec readback != written value");
    check(mode == 0, "mtvec MODE != 0 (direct)");
    check(base == (unsigned long)&d0_trap_entry,
          "mtvec BASE != &d0_trap_entry");
    check((base & 3UL) == 0, "trap entry not 4-byte aligned");

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

    // Trap 1: a deliberate M-mode ecall. The ecall's own address is
    // captured with an in-asm numeric local label so the handler's
    // recorded mepc can be checked against the true trap site.
    __asm__ volatile(
        "la %0, 1f\n"
        "1: ecall\n"
        : "=r"(ecall_addr)
        :
        : "memory");
    uart_puts("ecall: count=");
    uart_put_dec(ecall_count);
    uart_puts(" mcause=");
    uart_put_hex(ecall_mcause);
    uart_puts(" landing=");
    uart_put_hex(ecall_landing);
    uart_puts(" (expect ");
    uart_put_hex(base);
    uart_puts(") mepc=");
    uart_put_hex(ecall_mepc);
    uart_puts(" (expect ");
    uart_put_hex(ecall_addr);
    uart_puts(")\n");
    check(ecall_count == 1, "expected exactly one M-mode ecall trap");
    check(ecall_mcause == MCAUSE_ECALL_M, "ecall mcause != 11");
    check(ecall_landing == base,
          "ecall landing != BASE (not BASE+4*cause)");
    check(ecall_mepc == ecall_addr, "ecall mepc != ecall address");
    check(timer_count == 0, "timer fired during ecall phase");
    check(unexpected_count == 0, "unexpected trap during ecall phase");

    // Trap 2: machine timer interrupt, mcause
    // 0x8000000000000007. Asynchronous, but in direct mode it must
    // still land at the single BASE entry.
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
    uart_put_hex(base);
    uart_puts(")\n");
    check(!timed_out, "machine timer interrupt never delivered");
    check(timer_count == 1, "timer trap did not fire exactly once");
    check(timer_mcause == MCAUSE_MTI,
          "timer mcause != 0x8000000000000007");
    check(timer_landing == base,
          "timer landing != BASE (not BASE+4*cause)");
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

    // Checksum over the measured values that carry the claim: the
    // mtvec write/readback pair and both traps' mcause, landing, and
    // (for the ecall) mepc.
    checksum = 0xcbf29ce484222325UL;
    checksum = fnv1a(checksum, written);
    checksum = fnv1a(checksum, mtvec);
    checksum = fnv1a(checksum, ecall_mcause);
    checksum = fnv1a(checksum, ecall_landing);
    checksum = fnv1a(checksum, ecall_mepc);
    checksum = fnv1a(checksum, timer_mcause);
    checksum = fnv1a(checksum, timer_landing);
    uart_puts("checksum: fnv1a64=");
    uart_put_hex(checksum);
    uart_puts("\n");

    if (fails == 0) {
        uart_puts("RESULT: PASS (M-mode ecall->BASE mcause=11, timer->BASE mcause=0x8000000000000007, 0 traps elsewhere)\n");
    } else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");

    // Let the UART drain (TEMT: transmitter fully empty) before
    // touching the finisher device.
    while ((*UART0_LSR & LSR_TEMT) == 0)
        ;
    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    // FAIL: park the hart without touching the finisher device. The
    // harness runs QEMU under timeout, so a FAIL is observable as
    // the timeout exit status (124) as well as the RESULT line.
    for (;;)
        __asm__ volatile("wfi");
}
