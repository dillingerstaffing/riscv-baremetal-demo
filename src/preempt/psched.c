// psched.c: preemptive round-robin scheduler on the CLINT machine timer.
//
// The timer interrupt is the only scheduling event. Each tick: stamp the
// interrupt latency (mtime at trap entry minus the mtimecmp deadline that
// fired), close out the previous switch's cycle cost (its exit stamps are
// now known), rearm the timer, and pick the next task. After N_TICKS the
// handler switches back to the boot context saved at the first trap, which
// lets preempt_wait() return so the results can be printed.

#include "psched.h"
#include "clint.h"
#include "ptasks.h"
#include "../uart.h"

#define QUANTUM_TICKS 10000UL  // 1 ms per slice at the 10 MHz timebase
#define N_TICKS 200            // total timer ticks per run

// mcause for a machine timer interrupt: interrupt bit + cause code 7.
#define MCAUSE_TIMER_IRQ 0x8000000000000007UL

// mie.MTIE is bit 7; mstatus.MIE is bit 3.
#define MIE_MTIE (1UL << 7)
#define MSTATUS_MIE (1UL << 3)

static ptask_t tasks[PTASK_MAX];
static int n_tasks = 0;
static int current = -1;

// Boot context. The first timer trap saves preempt_wait()'s registers here;
// the final tick switches back to it so wait() can return.
static trapframe_t sched_tf;

volatile unsigned long g_exit_cycles = 0;
volatile unsigned long g_exit_mtime = 0;
static unsigned long g_entry_cycles = 0;
static unsigned long g_entry_mtime = 0;
static int have_prev = 0;

static volatile unsigned long ticks = 0;
static volatile int done = 0;
static unsigned long deadline = 0;  // mtimecmp value programmed per tick

// Latency stats, in mtime ticks (100 ns each).
static unsigned long lat_min = ~0UL, lat_max = 0, lat_sum = 0;
// Switch-cost stats, in rdcycle units and in mtime ticks.
static unsigned long swc_min = ~0UL, swc_max = 0, swc_sum = 0;
static unsigned long swt_min = ~0UL, swt_max = 0, swt_sum = 0;
static unsigned long swn = 0;

// Runs on a fresh task's stack the first time it is switched in.
static void ptask_trampoline(void) {
    tasks[current].fn();
    for (;;)  // a task body should never return; park it if it does
        __asm__ volatile("wfi");
}

void ptask_create(ptask_fn_t fn, const char *name) {
    ptask_t *t;

    if (n_tasks >= PTASK_MAX)
        return;  // table full: ignore the extra task

    t = &tasks[n_tasks++];
    for (int i = 0; i < 32; i++)
        t->tf.regs[i] = 0;
    // Stack grows down: start at the top, 16-byte aligned per the ABI.
    t->tf.regs[2] = ((unsigned long)(t->stack + PSTACK_SIZE)) & ~15UL;  // sp
    t->tf.mepc = (unsigned long)ptask_trampoline;
    t->tf.mtime_at_entry = 0;
    t->tf.cycle_at_entry = 0;
    t->fn = fn;
    t->name = name;
}

static unsigned long read_mcause(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mcause" : "=r"(v));
    return v;
}

static void timer_arm(void) {
    deadline = clint_get_mtime() + QUANTUM_TICKS;
    // If a handler ever ran longer than a whole quantum, skip ahead so the
    // still-pending interrupt does not re-fire immediately in a tight loop.
    while (deadline <= clint_get_mtime())
        deadline += QUANTUM_TICKS;
    clint_set_mtimecmp(deadline);
}

extern void trap_entry(void);

void preempt_init(void) {
    // Program the timer BEFORE enabling its interrupt. At reset mtimecmp
    // is zero, so mtime >= mtimecmp already holds and a timer interrupt is
    // pending; enabling MTIE/MIE first would trap immediately on that stale
    // pending bit instead of on our first real tick.
    __asm__ volatile("csrw mtvec, %0" :: "r"(trap_entry));     // direct mode
    __asm__ volatile("csrw mscratch, %0" :: "r"(&sched_tf));
    ticks = 0;
    done = 0;
    have_prev = 0;
    timer_arm();  // mtimecmp = now + quantum: clears the stale pending bit
    __asm__ volatile("csrrs zero, mie, %0" :: "r"(MIE_MTIE));      // MTIE
    __asm__ volatile("csrrs zero, mstatus, %0" :: "r"(MSTATUS_MIE));  // MIE
}

void preempt_wait(void) {
    while (!done)
        __asm__ volatile("wfi");
    // wfi cannot miss the wakeup: if the timer already fired, mip holds a
    // pending enabled interrupt and wfi returns without sleeping.
}

void preempt_stop(void) {
    __asm__ volatile("csrrc zero, mie, %0" :: "r"(MIE_MTIE));  // clear MTIE
}

trapframe_t *ptrap_handler(trapframe_t *old) {
    if (read_mcause() != MCAUSE_TIMER_IRQ) {
        uart_puts("\npreempt: unexpected trap (not a timer interrupt), halting\n");
        for (;;)
            __asm__ volatile("wfi");
    }
    ticks++;

    // Interrupt latency: mtime stamped at trap entry minus the mtimecmp
    // deadline that fired. The interrupt becomes pending when
    // mtime >= mtimecmp, so this difference is the hardware-to-handler
    // delay in 100 ns ticks.
    {
        unsigned long lat = old->mtime_at_entry - deadline;
        if (lat < lat_min)
            lat_min = lat;
        if (lat > lat_max)
            lat_max = lat;
        lat_sum += lat;
    }

    // Full cost of the previous switch: its exit stamps were written by the
    // trap.S exit path after this handler returned last time, so both
    // endpoints are known now.
    if (have_prev) {
        unsigned long dc = g_exit_cycles - g_entry_cycles;
        unsigned long dt = g_exit_mtime - g_entry_mtime;
        if (dc < swc_min)
            swc_min = dc;
        if (dc > swc_max)
            swc_max = dc;
        swc_sum += dc;
        if (dt < swt_min)
            swt_min = dt;
        if (dt > swt_max)
            swt_max = dt;
        swt_sum += dt;
        swn++;
    }

    timer_arm();

    trapframe_t *next;
    if (ticks >= N_TICKS) {
        done = 1;
        next = &sched_tf;  // hand the CPU back to preempt_wait()
    } else {
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
    unsigned long dc = g_exit_cycles - g_entry_cycles;
    unsigned long dt = g_exit_mtime - g_entry_mtime;
    if (dc < swc_min)
        swc_min = dc;
    if (dc > swc_max)
        swc_max = dc;
    swc_sum += dc;
    if (dt < swt_min)
        swt_min = dt;
    if (dt > swt_max)
        swt_max = dt;
    swt_sum += dt;
    swn++;
}

void preempt_report(void) {
    int i;

    account_final_switch();

    uart_puts("\npreempt: done. ");
    uart_put_dec(ticks);
    uart_puts(" timer ticks, ");
    uart_put_dec(swn);
    uart_puts(" measured context switches\n\n");

    uart_puts("task progress (task bodies never yield):\n");
    for (i = 0; i < ptask_count(); i++) {
        uart_puts("  ");
        uart_puts(ptask_name(i));
        uart_puts(": ");
        uart_put_dec(ptask_iters(i));
        uart_puts(" iterations\n");
    }

    uart_puts("\ninterrupt latency (mtime tick = 100 ns):\n  min ");
    uart_put_dec(lat_min);
    uart_puts(" / max ");
    uart_put_dec(lat_max);
    uart_puts(" / avg ");
    uart_put_dec(lat_sum / ticks);
    uart_puts(" ticks  (");
    uart_put_dec(lat_min * CLINT_NS_PER_TICK);
    uart_puts(" / ");
    uart_put_dec(lat_max * CLINT_NS_PER_TICK);
    uart_puts(" / ");
    uart_put_dec((lat_sum / ticks) * CLINT_NS_PER_TICK);
    uart_puts(" ns)\n");

    uart_puts("\ncontext-switch cost, entry stamp to exit stamp:\n  min ");
    uart_put_dec(swc_min);
    uart_puts(" / max ");
    uart_put_dec(swc_max);
    uart_puts(" / avg ");
    uart_put_dec(swc_sum / swn);
    uart_puts(" cycles\n  cross-check via mtime: min ");
    uart_put_dec(swt_min);
    uart_puts(" / max ");
    uart_put_dec(swt_max);
    uart_puts(" / avg ");
    uart_put_dec(swt_sum / swn);
    uart_puts(" ticks (");
    uart_put_dec(swt_min * CLINT_NS_PER_TICK);
    uart_puts(" / ");
    uart_put_dec(swt_max * CLINT_NS_PER_TICK);
    uart_puts(" / ");
    uart_put_dec((swt_sum / swn) * CLINT_NS_PER_TICK);
    uart_puts(" ns)\n");
}
