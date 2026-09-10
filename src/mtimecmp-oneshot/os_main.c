// os_main.c: mtimecmp one-shot disarm experiment (proof backlog item 148).
//
// A single machine timer interrupt is armed exactly one mtime tick
// ahead of the CLINT mtime (OS_AHEAD_TICKS = 1). The M-mode handler
// takes the trap, disarms the timer by writing all-ones to mtimecmp
// while still inside the handler, reads mip to confirm the MTIP
// pending bit dropped, and returns. After that one trap, with mstatus
// MIE and the mie MTIE bit still enabled, the hart spins through a
// quiet window of OS_QUIET_READS (1,000,000) rdcycle reads; the C
// handler counts every trap, so any re-delivery during the window is
// caught and fails the run.
//
// The verdict is PASS only if: exactly one trap fired, its mcause is
// the machine timer interrupt (0x8000000000000007), its mepc lies
// inside the trial spin loop (between os_loop and os_done), the mip
// MTIP bit reads clear after the disarm write, and the quiet window
// shows zero additional traps.
//
// The spin loop's trap and resume addresses are in-asm global labels;
// no C labels-as-values are used (distro gcc miscompiles &&label at
// -O2, documented in this project's memory).

#include "os.h"
#include "../preempt/clint.h"
#include "../uart.h"

os_save_t os_save;
volatile unsigned long os_trap_count;
volatile unsigned long os_flag;
volatile unsigned long os_mcause;
volatile unsigned long os_mepc;
volatile unsigned long os_mip_after_disarm;
volatile unsigned long os_bad_seen;
volatile unsigned long os_bad_mcause;
volatile unsigned long os_bad_mepc;
volatile unsigned long os_arm_cmp;
volatile unsigned long os_arm_clean;
volatile unsigned long os_quiet_traps;

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long rd_cycle(void) {
    unsigned long v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

static unsigned long read_mip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mip" : "=r"(v));
    return v;
}

// Trap handler: runs on the dedicated trap stack with all registers
// saved. The first trap records mcause/mepc, disarms the timer inside
// the handler (mtimecmp = all-ones), and confirms the mip MTIP bit
// dropped. Any further trap (there must be none) is recorded as bad.
// os_flag wakes the spin loop; mret resumes the interrupted code.
void os_trap_handler(os_save_t *s) {
    if (os_trap_count == 0) {
        os_mcause = s->mcause;
        os_mepc = s->mepc;
        clint_set_mtimecmp(~0UL);   // disarm inside the handler
        os_mip_after_disarm = read_mip();
    } else {
        os_bad_seen = 1;
        os_bad_mcause = s->mcause;
        os_bad_mepc = s->mepc;
    }
    os_trap_count++;
    os_flag = 1;
}

// The arming spin loop, emitted verbatim: enable MIE, then spin on
// os_flag (set by the handler) between the global os_loop/os_done
// labels, then disable MIE. Marked noinline so the global labels are
// defined exactly once.
__attribute__((noinline)) static void os_wait(void) {
    os_flag = 0;
    __asm__ volatile(
        "csrsi mstatus, 8\n"
        ".global os_loop\n"
        "os_loop:\n"
        "lw t0, 0(%0)\n"
        "beqz t0, os_loop\n"
        ".global os_done\n"
        "os_done:\n"
        "csrci mstatus, 8\n"
        :
        : "r"(&os_flag)
        : "t0", "memory");
}

// Arm: write mtimecmp = mtime + OS_AHEAD_TICKS (exactly one tick
// ahead), then record whether mtime was still below cmp when the write
// landed (the clean-arm statistic). Under this emulator the MMIO write
// plus the verify read spans more than one 100 ns tick, so the timer
// interrupt is typically already pending when MIE is enabled. The
// trap, the in-handler disarm, and the quiet-window measurements are
// identical either way, and the statistic documents the granularity
// rather than hiding it. The trap must fire exactly once; it cannot be
// missed, only already pending.
static void arm_one_shot(void) {
    unsigned long t = clint_get_mtime();
    unsigned long cmp = t + OS_AHEAD_TICKS;
    clint_set_mtimecmp(cmp);
    os_arm_cmp = cmp;
    os_arm_clean = (clint_get_mtime() < cmp);
    os_wait();
}

// Quiet window: MIE on, the machine timer interrupt still enabled in
// mie, mtimecmp = all-ones. OS_QUIET_READS rdcycle reads must produce
// zero traps. The handler counts any trap, so os_trap_count moving
// catches a re-delivery. The volatile sink keeps the reads from being
// folded away; the before/after rdcycle delta proves the window ran.
static unsigned long os_quiet_reads_done;
static volatile unsigned long os_sink;

static unsigned long quiet_window(void) {
    unsigned long start = rd_cycle();
    unsigned long i;
    __asm__ volatile("csrsi mstatus, 8");
    for (i = 0; i < OS_QUIET_READS; i++) {
        unsigned long v;
        __asm__ volatile("rdcycle %0" : "=r"(v));
        os_sink = v;
    }
    __asm__ volatile("csrci mstatus, 8");
    os_quiet_reads_done = OS_QUIET_READS;
    return rd_cycle() - start;
}

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U

int main(void) {
    unsigned long mtvec, quiet_delta, before_count;

    uart_init();
    uart_puts("mtimecmp-oneshot: one-shot disarm, quiet window (backlog item 148)\n");

    __asm__ volatile("csrw mtvec, %0" :: "r"(os_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(&os_save));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    uart_puts("mtvec=");
    uart_put_hex(mtvec);
    uart_puts(" mscratch=");
    uart_put_hex((unsigned long)&os_save);
    uart_puts("\n");
    check((mtvec & 3UL) == 0, "mtvec not in direct mode");

    clint_set_mtimecmp(~0UL); // disarmed until the single trial arms it
    // Only the machine timer interrupt is enabled in mie; mstatus.MIE
    // is set only inside the trial block and the quiet window, so a
    // trap can only land in those two regions.
    __asm__ volatile("csrs mie, %0" :: "r"(0x80UL));

    uart_puts("arming mtimecmp = mtime+");
    uart_put_dec(OS_AHEAD_TICKS);
    uart_puts(" tick\n");

    arm_one_shot();
    uart_puts("arm: mtimecmp=");
    uart_put_hex(os_arm_cmp);
    uart_puts(" (mtime+1), clean=");
    uart_put_dec(os_arm_clean);
    uart_puts(" (1 = mtime still below cmp when the write landed)\n");

    // The single trap, before the quiet window.
    uart_puts("trap_count=");
    uart_put_dec(os_trap_count);
    uart_puts(" mcause=");
    uart_put_hex(os_mcause);
    uart_puts(" mepc=");
    uart_put_hex(os_mepc);
    uart_puts("\n");
    uart_puts("spin loop bounds: os_loop=");
    uart_put_hex((unsigned long)os_loop);
    uart_puts(" os_done=");
    uart_put_hex((unsigned long)os_done);
    uart_puts("\n");
    uart_puts("mip after disarm=");
    if (os_trap_count == 1) {
        uart_put_hex(os_mip_after_disarm);
        uart_puts(" (MTIP bit ");
        uart_puts((os_mip_after_disarm & MIP_MTIP) ? "SET" : "clear");
        uart_puts(")\n");
    } else {
        uart_puts("(no trap fired, no disarm happened)\n");
    }

    check(os_trap_count == 1, "trap count != 1 after arming");
    check(os_mcause == MCAUSE_MTI, "mcause != machine timer interrupt");
    check(os_mepc >= (unsigned long)os_loop &&
          os_mepc < (unsigned long)os_done,
          "mepc outside the trial spin loop");
    check((os_mip_after_disarm & MIP_MTIP) == 0,
          "mip MTIP still set after disarm write");

    // Quiet window: disarmed, interrupts on, no trap may fire.
    uart_puts("quiet window: ");
    uart_put_dec(OS_QUIET_READS);
    uart_puts(" rdcycle reads with MIE on, mtimecmp=all-ones\n");
    before_count = os_trap_count;
    quiet_delta = quiet_window();
    os_quiet_traps = os_trap_count - before_count;
    uart_puts("quiet window rdcycle delta=");
    uart_put_dec(quiet_delta);
    uart_puts(" (reads done=");
    uart_put_dec(os_quiet_reads_done);
    uart_puts(")\n");
    uart_puts("traps in quiet window=");
    uart_put_dec(os_quiet_traps);
    uart_puts("\n");

    check(os_quiet_traps == 0, "re-delivery trap fired in quiet window");
    check(os_trap_count == 1, "trap count != 1 after quiet window");
    if (os_bad_seen) {
        uart_puts("unexpected extra trap: mcause=");
        uart_put_hex(os_bad_mcause);
        uart_puts(" mepc=");
        uart_put_hex(os_bad_mepc);
        uart_puts("\n");
    }

    // Verdict section: the byte-identical logical result lines.
    uart_puts("VERDICT trap_count=");
    uart_put_dec(os_trap_count);
    uart_puts(" mcause=");
    uart_put_hex(os_mcause);
    uart_puts(" mepc=");
    uart_put_hex(os_mepc);
    uart_puts(" quiet_window_reads=");
    uart_put_dec(OS_QUIET_READS);
    uart_puts(" quiet_window_traps=");
    uart_put_dec(os_quiet_traps);
    uart_puts("\n");

    if (fails == 0)
        uart_puts("RESULT: PASS\n");
    else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }

    // Let the UART drain before touching the finisher device.
    {
        unsigned long drain = clint_get_mtime();
        while (clint_get_mtime() - drain < 100000UL)
            ;
    }

    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS; // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    // FAIL: park the hart without touching the finisher device. The
    // harness runs QEMU under timeout, so a FAIL is observable as
    // the timeout exit status (124) as well as the RESULT line.
    for (;;)
        __asm__ volatile("wfi");
}
