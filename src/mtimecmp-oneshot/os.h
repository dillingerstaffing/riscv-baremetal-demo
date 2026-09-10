// os.h: shared definitions for the mtimecmp one-shot disarm experiment.
//
// One machine timer interrupt is armed exactly one mtime tick ahead of
// the CLINT mtime. The M-mode handler takes the trap, disarms the timer
// by writing all-ones to mtimecmp while still inside the handler, and
// returns to the interrupted code. After that single trap, with MIE and
// the machine timer interrupt still enabled, the hart spins through a
// quiet window of OS_QUIET_READS rdcycle reads and the experiment
// requires that zero further traps fire.
//
// This isolates the disarm path: the companion src/mtimecmp module
// measures delivery accuracy over 1000 armed trials; this module
// measures only what happens after the disarm write.

#ifndef OS_H
#define OS_H

// Machine timer interrupt: mcause interrupt bit set, code 7.
#define MCAUSE_MTI 0x8000000000000007UL

// mip MTIP bit (bit 7): the pending bit the timer interrupt sets.
#define MIP_MTIP 0x80UL

// One-shot arming: mtimecmp = mtime + 1, exactly one mtime tick ahead.
#define OS_AHEAD_TICKS 1UL

// Quiet window length in rdcycle reads; must see zero re-delivery traps.
#define OS_QUIET_READS 1000000UL

// The arm write is mtime = mtime+1; whether mtime was still below the
// written value when the write landed is measured, not assumed. Under
// this emulator the MMIO write plus the verify read spans more than
// one 100 ns tick, so the interrupt is typically already pending when
// MIE is enabled; the disarm path is identical either way.

// Trap save area. Offsets must match os_trap.S:
//   t0_save@0 t1_save@8 mcause@16 mepc@24,
//   then ra,sp,gp,tp,t2,s0,s1,a0-a7,s2-s11,t3-t6 at 48,56,...,272.
typedef struct {
    unsigned long t0_save;
    unsigned long t1_save;
    unsigned long mcause;
    unsigned long mepc;
    unsigned long gpr[29];
} os_save_t;

extern os_save_t os_save;
extern volatile unsigned long os_trap_count;
extern volatile unsigned long os_flag;
extern volatile unsigned long os_mcause;          // first trap's mcause
extern volatile unsigned long os_mepc;            // first trap's mepc
extern volatile unsigned long os_mip_after_disarm; // mip read in the handler
extern volatile unsigned long os_bad_seen;        // any trap beyond the first
extern volatile unsigned long os_bad_mcause;
extern volatile unsigned long os_bad_mepc;
extern volatile unsigned long os_arm_cmp;         // the written mtimecmp value
extern volatile unsigned long os_arm_clean;       // 1 if mtime < cmp at write
extern volatile unsigned long os_quiet_traps;     // traps fired in the quiet window

// Labels bounding the spin loop inside the arming asm block (os_main.c):
// the one timer trap can only land between them while MIE is on.
extern char os_loop[];
extern char os_done[];

void os_trap_entry(void);
void os_trap_handler(os_save_t *s);

#endif // OS_H
