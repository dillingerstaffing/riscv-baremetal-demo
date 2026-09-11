// stos.h: shared definitions for the stimecmp one-shot disarm experiment.
//
// A single supervisor timer interrupt is armed 1000 mtime ticks ahead of
// the CLINT mtime (Sstc extension, stimecmp CSR). The S-mode handler takes
// the trap, disarms the timer by writing all-ones to stimecmp while still
// inside the handler, and returns. After that single trap, with sstatus.SIE
// and the sie STIE bit still enabled, the hart spins through a quiet window
// of STOS_QUIET_READS rdcycle reads and the experiment requires that zero
// further traps fire.
//
// This is the S-mode analog of the shipped src/mtimecmp-oneshot module:
// that module proves the same one-shot property for the machine timer
// interrupt (mtimecmp, M-mode); this module proves it for the supervisor
// timer interrupt (stimecmp, S-mode) with mideleg delegation. The boot,
// trap entry, handler, and reporting code are written separately; the two
// modules share only boot.S and the UART driver via the Makefile.

#ifndef STOS_H
#define STOS_H

// Supervisor timer interrupt: scause interrupt bit set, code 5.
#define SCAUSE_STI 0x8000000000000005UL

// sip STIP bit (bit 5): the pending bit the supervisor timer sets.
#define SIP_STIP 0x20UL

// One-shot arming: stimecmp = mtime + 1000 mtime ticks.
#define STOS_AHEAD_TICKS 1000UL

// Quiet window length in rdcycle reads; must see zero re-delivery traps.
#define STOS_QUIET_READS 1000000UL

// stimecmp (Sstc) CSR number; menvcfg bit 63 (STCE) enables S-mode access.
#define CSR_STIMECMP 0x14d
#define MENVCFG_STCE (1UL << 63)

// mideleg bit 5 (STI): delegate the supervisor timer interrupt to S-mode.
#define MIDELEG_STI (1UL << 5)

// CLINT mtimecmp for hart 0 on the QEMU virt board (M-mode only).
#define CLINT_MTIMECMP 0x02004000UL

// Trap save area. Offsets must match stos_trap.S:
//   t0_save@0 t1_save@8 scause@16 sepc@24,
//   then ra,sp,gp,tp,t2,s0,s1,a0-a7,s2-s11,t3-t6 at 48,56,...,272.
typedef struct {
    unsigned long t0_save;
    unsigned long t1_save;
    unsigned long scause;
    unsigned long sepc;
    unsigned long gpr[29];
} stos_save_t;

extern stos_save_t stos_save;
extern volatile unsigned long stos_trap_count;
extern volatile unsigned long stos_flag;
extern volatile unsigned long stos_scause;           // first trap's scause
extern volatile unsigned long stos_sepc;             // first trap's sepc
extern volatile unsigned long stos_sip_after_disarm; // sip read in the handler
extern volatile unsigned long stos_cmp_after_disarm; // stimecmp read in handler
extern volatile unsigned long stos_bad_seen;         // any trap beyond the first
extern volatile unsigned long stos_bad_scause;
extern volatile unsigned long stos_bad_sepc;
extern volatile unsigned long stos_arm_cmp;         // the written stimecmp value
extern volatile unsigned long stos_arm_clean;       // 1 if mtime < cmp at write
extern volatile unsigned long stos_quiet_traps;     // traps fired in the quiet window
extern volatile unsigned long stos_m_writes;        // stimecmp writes from M-mode
extern volatile unsigned long stos_s_writes;        // stimecmp writes from S-mode
extern volatile unsigned long stos_mideleg;         // mideleg readback
extern volatile unsigned long stos_stce;            // menvcfg.STCE probe result
extern volatile unsigned long stos_quiet_reads_done;

// Labels bounding the spin loop inside the arming asm block (stos_main.c):
// the one timer trap can only land between them while SIE is on.
extern char stos_loop[];
extern char stos_done[];

void stos_trap_entry(void);
void stos_trap_handler(stos_save_t *s);

// S-mode entry point: M-mode boot finishes with mret to s_main.
void s_main(void);

#endif // STOS_H
