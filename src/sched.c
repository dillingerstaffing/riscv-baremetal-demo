// sched.c: cooperative round-robin scheduler with real context switches.
// A new task starts life with a faked register context: its stack pointer
// aims at its private stack and its return address aims at task_trampoline,
// so the first switch into it simply "returns" into the task body.

#include "sched.h"

static task_t tasks[MAX_TASKS];
static int num_tasks = 0;
static task_t *current = 0;
static context_t sched_ctx;  // the scheduler's own saved context

static void task_trampoline(void);

void task_create(task_fn_t fn, const char *name) {
    task_t *t;

    if (num_tasks >= MAX_TASKS)
        return;  // table full: ignore the extra task

    t = &tasks[num_tasks++];

    // Stack grows down: start at the top, 16-byte aligned per the ABI.
    t->ctx.sp = ((unsigned long)(t->stack + STACK_SIZE)) & ~15UL;
    t->ctx.ra = (unsigned long)task_trampoline;
    t->fn = fn;
    t->name = name;
    t->state = TASK_READY;
}

// Runs on the new task's stack the first time it is switched in.
static void task_trampoline(void) {
    current->fn();  // run the task body
    current->state = TASK_DONE;
    for (;;)
        sched_yield();  // never return into a finished context
}

void sched_yield(void) {
    swtch(&current->ctx, &sched_ctx);
}

void sched_run(void) {
    int remaining = num_tasks;

    while (remaining > 0) {
        for (int i = 0; i < num_tasks; i++) {
            task_t *t = &tasks[i];

            if (t->state == TASK_DONE)
                continue;
            current = t;
            swtch(&sched_ctx, &t->ctx);  // runs until it yields or exits
            if (t->state == TASK_DONE)
                remaining--;
        }
    }
}
