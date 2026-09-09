// wfi.h: shared definitions for the WFI wakeup-latency module.
//
// The experiment measures, per trial, the mtime tick at which a CLINT
// timer interrupt was programmed (mtimecmp), the mtime/rdcycle stamps
// taken in the trap entry, and the stamps taken by the first two
// instructions executed after mret. All latency metrics are derived
// from stamps in the same clock domain: tick-domain metrics use only
// rdtime stamps, rdcycle-domain metrics use only rdcycle stamps.

#ifndef WFI_H
#define WFI_H

// Machine timer interrupt: mcause interrupt bit set, code 7.
#define MCAUSE_MTI 0x8000000000000007UL

// Number of trials per kind (wfi and spin), and how many leading trials
// of each kind are discarded from the statistics (icache/TLB warmup).
#define WFI_NTRIALS 400
#define WFI_WARMUP 8

// How far ahead of the current mtime each trial's mtimecmp is programmed,
// in mtime ticks (nominally 100 ns each). 20000 ticks = 2 ms: long enough
// that ordinary host descheduling of the QEMU vcpu cannot stretch the
// arm-to-trial window past the deadline (the run fails loudly if it does).
#define WFI_AHEAD_TICKS 20000UL

// Trap save area. Offsets must match wfi_trap.S:
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
} wfi_save_t;

// One trial's stamps. cmp/t_pre/c_pre are recorded by main around the
// arming; the trap entry records mcause/mepc_before/t_entry/c_entry;
// the two instructions after mret record c_resume/t_resume.
typedef struct {
    unsigned long cmp;
    unsigned long t_pre;
    unsigned long c_pre;
    unsigned long mcause;
    unsigned long mepc_before;
    unsigned long t_entry;
    unsigned long c_entry;
    unsigned long c_resume;
    unsigned long t_resume;
} trial_rec_t;

extern wfi_save_t wfi_save;
extern trial_rec_t wfi_rec[WFI_NTRIALS];
extern trial_rec_t spin_rec[WFI_NTRIALS];
extern volatile unsigned long wfi_current_kind; // 0 = wfi trial, 1 = spin trial
extern volatile unsigned long wfi_trial_idx;
extern volatile unsigned long wfi_spin_flag;
extern volatile unsigned long wfi_trap_count;

// Resume labels defined inside the trial asm blocks (wfi_main.c).
extern char wfi_resume[];
extern char spin_resume[];
extern char spin_loop[];

void wfi_trap_entry(void);
void wfi_trap_handler(wfi_save_t *s);

#endif // WFI_H
