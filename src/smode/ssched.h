// ssched.h: preemptive scheduler for the smode module, built twice.
//
// TRAP_SMODE=1 (smode.elf): the scheduler runs in S-mode on the delegated
//   supervisor timer interrupt; traps enter through strap.S on the S-mode
//   CSRs and exit with sret.
// TRAP_SMODE=0 (smode-mbase.elf): the identical workload runs in M-mode on
//   the machine timer interrupt; this is the baseline the delegation
//   overhead is measured against.
//
// A trapframe holds the full interrupted context: all 31 writable integer
// registers plus the exception PC. On a timer tick the assembly trap entry
// saves the outgoing context into its trapframe, the C handler rearms the
// timer and picks the next task, and the exit path restores the incoming
// context and returns from the trap. Tasks never yield; the timer does all
// of the switching.

#ifndef SSCHED_H
#define SSCHED_H

#define STASK_MAX 3
#define SSTACK_SIZE 4096

// Field offsets must match strap.S: regs[0..31] at 0..248, epc at 256,
// mtime_at_entry at 264, cycle_at_entry at 272.
typedef struct {
    unsigned long regs[32];      // x0..x31 (x0 kept as a zero slot so the
                                 // register number indexes the array)
    unsigned long epc;           // where the context resumes
    unsigned long mtime_at_entry;  // rdtime stamped at trap entry
    unsigned long cycle_at_entry;  // rdcycle stamped at trap entry
} trapframe_t;

typedef void (*stask_fn_t)(void);

typedef struct {
    trapframe_t tf;
    unsigned char stack[SSTACK_SIZE] __attribute__((aligned(16)));
    stask_fn_t fn;
    const char *name;
} stask_t;

void stask_create(stask_fn_t fn, const char *name);

// Program the trap vector and scratch register, enable the timer
// interrupt, arm the first tick. After this returns, sched_wait() sleeps
// in wfi until the timer handler has run its full quota of ticks.
void sched_init(void);
void sched_wait(void);
void sched_stop(void);

// Print the measured results. Call after sched_wait() returns.
void sched_report(void);

// Trap handler called from strap.S with the just-saved trapframe.
// Returns the trapframe to resume.
trapframe_t *strap_handler(trapframe_t *old);

// Stamped by the strap.S exit path just before the register restore, so
// the next handler invocation can close out the previous switch's cost.
extern volatile unsigned long g_exit_cycles;
extern volatile unsigned long g_exit_mtime;

// Boot context. The first timer trap saves sched_wait()'s registers here;
// the final tick switches back to it so wait() can return. m_boot (S-mode
// binary) points sscratch at this before dropping to S-mode.
extern trapframe_t sched_tf;

#endif  // SSCHED_H
