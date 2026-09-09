// ssched.c: preemptive round-robin scheduler, built twice (S-mode and the
// M-mode baseline) with the identical workload and measurement code. Only
// the trap privilege level and the timer rearm path differ.
//
// The timer interrupt is the only scheduling event. Each tick: stamp the
// interrupt latency (mtime at trap entry minus the comparator deadline
// that fired), close out the previous switch's cycle cost (its exit stamps
// are now known), rearm the timer, and pick the next task. After N_TICKS
// the handler switches back to the boot context saved at the first trap,
// which lets sched_wait() return so the results can be printed.

#include "ssched.h"
#include "stimer.h"
#include "stasks.h"
#include "../uart.h"

#define QUANTUM_TICKS 10000UL  // 1 ms per slice at the 10 MHz timebase
#define N_TICKS 200            // total timer ticks per run

#if TRAP_SMODE
// scause for a supervisor timer interrupt: interrupt bit + cause code 5.
#define CAUSE_TIMER_IRQ 0x8000000000000005UL
#define IE_TIE (1UL << 5)      // sie.STIE
#define STATUS_IE (1UL << 1)   // sstatus.SIE
#else
// mcause for a machine timer interrupt: interrupt bit + cause code 7.
#define CAUSE_TIMER_IRQ 0x8000000000000007UL
#define IE_TIE (1UL << 7)      // mie.MTIE
#define STATUS_IE (1UL << 3)   // mstatus.MIE
#endif

static stask_t tasks[STASK_MAX];
static int n_tasks = 0;
static int current = -1;

trapframe_t sched_tf;

volatile unsigned long g_exit_cycles = 0;
volatile unsigned long g_exit_mtime = 0;
static unsigned long g_entry_cycles = 0;
static unsigned long g_entry_mtime = 0;
static int have_prev = 0;

static volatile unsigned long ticks = 0;
static volatile int done = 0;

// Per-tick samples: latency in mtime ticks (100 ns each), switch cost in
// rdcycle units and in mtime ticks.
static unsigned long lat[N_TICKS];
static unsigned long swc[N_TICKS];
static unsigned long swt[N_TICKS];
static unsigned long swn = 0;

// Runs on a fresh task's stack the first time it is switched in.
static void stask_trampoline(void) {
    tasks[current].fn();
    for (;;)  // a task body should never return; park it if it does
        __asm__ volatile("wfi");
}

void stask_create(stask_fn_t fn, const char *name) {
    stask_t *t;

    if (n_tasks >= STASK_MAX)
        return;  // table full: ignore the extra task

    t = &tasks[n_tasks++];
    for (int i = 0; i < 32; i++)
        t->tf.regs[i] = 0;
    // Stack grows down: start at the top, 16-byte aligned per the ABI.
    t->tf.regs[2] = ((unsigned long)(t->stack + SSTACK_SIZE)) & ~15UL;  // sp
    t->tf.epc = (unsigned long)stask_trampoline;
    t->tf.mtime_at_entry = 0;
    t->tf.cycle_at_entry = 0;
    t->fn = fn;
    t->name = name;
}

static unsigned long read_cause(void) {
    unsigned long v;
#if TRAP_SMODE
    __asm__ volatile("csrr %0, scause" : "=r"(v));
#else
    __asm__ volatile("csrr %0, mcause" : "=r"(v));
#endif
    return v;
}

extern void strap_entry(void);

void sched_init(void) {
    // Program the timer BEFORE enabling its interrupt. A stale pending
    // bit (mtimecmp reset to zero, or stimecmp parked at ~0) must not
    // fire before the first real deadline is armed.
#if TRAP_SMODE
    __asm__ volatile("csrw stvec, %0" :: "r"(strap_entry));  // direct mode
    __asm__ volatile("csrw sscratch, %0" :: "r"(&sched_tf));
#else
    __asm__ volatile("csrw mtvec, %0" :: "r"(strap_entry));  // direct mode
    __asm__ volatile("csrw mscratch, %0" :: "r"(&sched_tf));
#endif
    ticks = 0;
    done = 0;
    have_prev = 0;
    swn = 0;
    stimer_arm();  // arms the first real deadline
#if TRAP_SMODE
    __asm__ volatile("csrrs zero, sie, %0" :: "r"(IE_TIE));      // STIE
    __asm__ volatile("csrrs zero, sstatus, %0" :: "r"(STATUS_IE));  // SIE
#else
    __asm__ volatile("csrrs zero, mie, %0" :: "r"(IE_TIE));      // MTIE
    __asm__ volatile("csrrs zero, mstatus, %0" :: "r"(STATUS_IE));  // MIE
#endif
}

void sched_wait(void) {
    // Spin, do not wfi: the final tick does not rearm the timer (so no
    // tick past the quota can fire), which means a wfi here could sleep
    // past the end of the run with nothing left to wake it. Spinning
    // terminates deterministically on done. The timer interrupt still
    // preempts the spin, so the workload is unaffected.
    while (!done)
        __asm__ volatile("" ::: "memory");
}

void sched_stop(void) {
#if TRAP_SMODE
    __asm__ volatile("csrrc zero, sie, %0" :: "r"(IE_TIE));  // clear STIE
#else
    __asm__ volatile("csrrc zero, mie, %0" :: "r"(IE_TIE));  // clear MTIE
#endif
}

trapframe_t *strap_handler(trapframe_t *old) {
    if (read_cause() != CAUSE_TIMER_IRQ) {
        uart_puts("\nsmode: unexpected trap (not a timer interrupt), halting\n");
        for (;;)
            __asm__ volatile("wfi");
    }

    // Defensive: the disarm-on-final-tick logic below means no tick past
    // N_TICKS can fire, so this is unreachable; it guards the sample
    // arrays if a stray tick ever arrived. Disarm first so the stale
    // pending bit cannot re-trap us on return.
    if (ticks >= N_TICKS) {
        stimer_disarm();
        done = 1;
        return &sched_tf;
    }

    // Interrupt latency: mtime stamped at trap entry minus the comparator
    // deadline that fired. The interrupt becomes pending when
    // mtime >= deadline, so this difference is the hardware-to-handler
    // delay in 100 ns ticks.
    lat[ticks] = old->mtime_at_entry - stimer_deadline();

    // Full cost of the previous switch: its exit stamps were written by
    // the strap.S exit path after this handler returned last time, so
    // both endpoints are known now.
    if (have_prev) {
        swc[swn] = g_exit_cycles - g_entry_cycles;
        swt[swn] = g_exit_mtime - g_entry_mtime;
        swn++;
    }

    trapframe_t *next;
    ticks++;
    if (ticks >= N_TICKS) {
        done = 1;
        next = &sched_tf;  // hand the CPU back to sched_wait()
        // Quota met: DISARM the timer instead of rearming it. The old
        // deadline is in the past, so without this the pending bit
        // would stay set and the sret below would immediately re-trap.
        // After the disarm no tick past N_TICKS can fire, the sample
        // arrays cannot overflow, and sched_wait()'s spin (not wfi)
        // terminates deterministically on done.
        stimer_disarm();
    } else {
        stimer_arm();
        current = (current + 1) % n_tasks;
        next = &tasks[current].tf;
    }

    g_entry_cycles = old->cycle_at_entry;
    g_entry_mtime = old->mtime_at_entry;
    have_prev = 1;
    return next;
}

// Fold in the final switch (task -> boot context), whose exit stamps were
// written after the last handler invocation. Must run with the timer off.
static void account_final_switch(void) {
    swc[swn] = g_exit_cycles - g_entry_cycles;
    swt[swn] = g_exit_mtime - g_entry_mtime;
    swn++;
}

// Insertion sort of n unsigned longs; N_TICKS is small enough that this
// is trivially fast.
static void sort_copy(unsigned long *dst, unsigned long *src, unsigned long n) {
    unsigned long i, j, v;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
    for (i = 1; i < n; i++) {
        v = dst[i];
        j = i;
        while (j > 0 && dst[j - 1] > v) {
            dst[j] = dst[j - 1];
            j--;
        }
        dst[j] = v;
    }
}

static unsigned long median_of(unsigned long *a, unsigned long n) {
    static unsigned long buf[N_TICKS];
    sort_copy(buf, a, n);
    // Even count: average the two middle samples.
    return (buf[n / 2 - 1] + buf[n / 2]) / 2;
}

static unsigned long min_of(unsigned long *a, unsigned long n) {
    unsigned long m = a[0], i;
    for (i = 1; i < n; i++)
        if (a[i] < m)
            m = a[i];
    return m;
}

static unsigned long max_of(unsigned long *a, unsigned long n) {
    unsigned long m = a[0], i;
    for (i = 1; i < n; i++)
        if (a[i] > m)
            m = a[i];
    return m;
}

void sched_report(void) {
    int i;

    account_final_switch();

    uart_puts("\nsmode: done. ");
    uart_put_dec(ticks);
    uart_puts(" timer ticks, ");
    uart_put_dec(swn);
    uart_puts(" measured context switches\n\n");

    uart_puts("task progress (task bodies never yield):\n");
    for (i = 0; i < stask_count(); i++) {
        uart_puts("  ");
        uart_puts(stask_name(i));
        uart_puts(": ");
        uart_put_dec(stask_iters(i));
        uart_puts(" iterations\n");
    }

    uart_puts("\ninterrupt latency (mtime tick = 100 ns):\n  min ");
    uart_put_dec(min_of(lat, ticks));
    uart_puts(" / median ");
    uart_put_dec(median_of(lat, ticks));
    uart_puts(" / max ");
    uart_put_dec(max_of(lat, ticks));
    uart_puts(" ticks  (");
    uart_put_dec(min_of(lat, ticks) * STIMER_NS_PER_TICK);
    uart_puts(" / ");
    uart_put_dec(median_of(lat, ticks) * STIMER_NS_PER_TICK);
    uart_puts(" / ");
    uart_put_dec(max_of(lat, ticks) * STIMER_NS_PER_TICK);
    uart_puts(" ns)\n");

    uart_puts("\ncontext-switch cost, entry stamp to exit stamp:\n  min ");
    uart_put_dec(min_of(swc, swn));
    uart_puts(" / median ");
    uart_put_dec(median_of(swc, swn));
    uart_puts(" / max ");
    uart_put_dec(max_of(swc, swn));
    uart_puts(" cycles\n  cross-check via mtime: min ");
    uart_put_dec(min_of(swt, swn));
    uart_puts(" / median ");
    uart_put_dec(median_of(swt, swn));
    uart_puts(" / max ");
    uart_put_dec(max_of(swt, swn));
    uart_puts(" ticks (");
    uart_put_dec(min_of(swt, swn) * STIMER_NS_PER_TICK);
    uart_puts(" / ");
    uart_put_dec(median_of(swt, swn) * STIMER_NS_PER_TICK);
    uart_puts(" / ");
    uart_put_dec(max_of(swt, swn) * STIMER_NS_PER_TICK);
    uart_puts(" ns)\n");

    // Sanity cross-check of the cycle counter against mtime: the median
    // switch cost in cycles divided by the median in mtime ticks should
    // be the constant counter ratio observed on this QEMU build.
    uart_puts("\nrdcycle/rdtime ratio at median switch cost: ");
    uart_put_dec(median_of(swc, swn) / median_of(swt, swn));
    uart_puts(" cycles per 100 ns tick\n");
}
