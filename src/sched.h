// sched.h: tiny cooperative round-robin scheduler for bare-metal RISC-V.
// Each task owns a private stack. Switching tasks saves and restores the
// callee-saved registers (see switch.S); scheduling policy is simple
// round-robin over the task table.

#ifndef SCHED_H
#define SCHED_H

#define MAX_TASKS 3
#define STACK_SIZE 4096

typedef void (*task_fn_t)(void);

enum { TASK_READY = 0, TASK_DONE = 1 };

// Saved register context. Field order must match switch.S offsets.
typedef struct {
    unsigned long ra;
    unsigned long sp;
    unsigned long s0;
    unsigned long s1;
    unsigned long s2;
    unsigned long s3;
    unsigned long s4;
    unsigned long s5;
    unsigned long s6;
    unsigned long s7;
    unsigned long s8;
    unsigned long s9;
    unsigned long s10;
    unsigned long s11;
} context_t;

typedef struct {
    context_t ctx;                 // saved registers
    unsigned char stack[STACK_SIZE]; // private stack, grows down
    task_fn_t fn;                  // task body
    const char *name;
    int state;                     // TASK_READY or TASK_DONE
} task_t;

// Register a task before calling sched_run().
void task_create(task_fn_t fn, const char *name);

// Give up the CPU to the next ready task. Called from task bodies.
void sched_yield(void);

// Run all registered tasks round-robin until every task is done.
void sched_run(void);

// Assembly context switch: save *old_ctx, restore *new_ctx.
void swtch(context_t *old_ctx, context_t *new_ctx);

#endif  // SCHED_H
