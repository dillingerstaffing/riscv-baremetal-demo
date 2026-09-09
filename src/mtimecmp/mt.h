// mt.h: shared definitions for the mtimecmp accuracy experiment.
//
// Each trial programs the CLINT mtimecmp register N mtime ticks ahead
// of the current mtime, enables only the machine timer interrupt, and
// spins until the trap fires. The trap entry stamps rdtime (the tick
// domain, the primary measurement) and rdcycle (used to characterize
// the cycle counter). The headline metric per trial is
//   offset_ticks = t_entry - cmp,
// the number of mtime ticks by which actual trap delivery overshoots
// the programmed value. Cycle units are the tick metric scaled by the
// rdcycle-per-tick ratio measured in-module during calibration, never
// by mixing the two counters directly.

#ifndef MT_H
#define MT_H

// Machine timer interrupt: mcause interrupt bit set, code 7.
#define MCAUSE_MTI 0x8000000000000007UL

// Trial counts and arming. MT_AHEAD_TICKS = 5000 ticks = 500 us at the
// 10 MHz CLINT timebase. MT_OFFSET_BOUND is the delivery sanity bound:
// a trial whose delivery overshoots it (host descheduled the vcpu
// mid-trial) is re-run, not recorded.
#define MT_NTRIALS 1000
#define MT_WARMUP 8
#define MT_AHEAD_TICKS 5000UL
#define MT_OFFSET_BOUND 1000UL
#define MT_MAX_ATTEMPTS 100

// Trap save area. Offsets must match mt_trap.S:
//   t0_save@0 t1_save@8 mcause@16 mepc@24 mtime_entry@32 cycle_entry@40,
//   then ra,sp,gp,tp,t2,s0,s1,a0-a7,s2-s11,t3-t6 at 48,56,...,272.
typedef struct {
    unsigned long t0_save;
    unsigned long t1_save;
    unsigned long mcause;
    unsigned long mepc;
    unsigned long mtime_entry;
    unsigned long cycle_entry;
    unsigned long gpr[29];
} mt_save_t;

// One trial's record. cmp/t_pre/c_pre are recorded around the arming;
// the trap entry records mcause/mepc_before/t_entry/c_entry.
typedef struct {
    unsigned long cmp;
    unsigned long t_pre;
    unsigned long c_pre;
    unsigned long mcause;
    unsigned long mepc_before;
    unsigned long t_entry;
    unsigned long c_entry;
} mt_rec_t;

extern mt_save_t mt_save;
extern mt_rec_t mt_rec[MT_NTRIALS];
extern volatile unsigned long mt_trial_idx;
extern volatile unsigned long mt_flag;
extern volatile unsigned long mt_trap_count;

// Labels bounding the spin loop inside the trial asm block (mt_main.c):
// a trap can only land between them while MIE is on.
extern char mt_loop[];
extern char mt_done[];

void mt_trap_entry(void);
void mt_trap_handler(mt_save_t *s);

#endif // MT_H
