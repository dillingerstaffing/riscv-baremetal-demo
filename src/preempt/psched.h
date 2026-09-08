// psched.h: preemptive scheduler driven by the CLINT machine timer.
//
// A trapframe holds the full interrupted context: all 31 writable integer
// registers plus mepc. On a timer tick the assembly trap entry (trap.S)
// saves the outgoing context into its trapframe, a C handler rearms the
// timer and picks the next task, and the exit path restores the incoming
// context and executes mret. Tasks never yield; the timer does all of the
// switching.

#ifndef PSCHED_H
#define PSCHED_H

#define PTASK_MAX 3
#define PSTACK_SIZE 4096

// Field offsets must match trap.S: regs[0..31] at 0..248, mepc at 256,
// mtime_at_entry at 264, cycle_at_entry at 272.
typedef struct {
    unsigned long regs[32];      // x0..x31 (x0 kept as a zero slot so the
                                // register number indexes the array)
    unsigned long mepc;         // where the context resumes
    unsigned long mtime_at_entry;  // rdtime stamped at trap entry
    unsigned long cycle_at_entry;  // rdcycle stamped at trap entry
} trapframe_t;

typedef void (*ptask_fn_t)(void);

typedef struct {
    trapframe_t tf;
    unsigned char stack[PSTACK_SIZE] __attribute__((aligned(16)));
    ptask_fn_t fn;
    const char *name;
} ptask_t;

void ptask_create(ptask_fn_t fn, const char *name);

// Program mtvec/mscratch, enable the machine timer interrupt, arm the
// first tick. After this returns, preempt_wait() sleeps in wfi until the
// timer handler has run its full quota of ticks.
void preempt_init(void);
void preempt_wait(void);
void preempt_stop(void);

// Print the measured results. Call after preempt_wait() returns.
void preempt_report(void);

// Trap handler called from trap.S with the just-saved trapframe.
// Returns the trapframe to resume.
trapframe_t *ptrap_handler(trapframe_t *old);

// Stamped by the trap.S exit path just before the register restore, so the
// next handler invocation can close out the previous switch's cost.
extern volatile unsigned long g_exit_cycles;
extern volatile unsigned long g_exit_mtime;

#endif  // PSCHED_H
