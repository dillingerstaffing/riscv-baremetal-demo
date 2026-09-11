// stg.h: shared definitions for the sie.STIE gate experiment.
//
// One mechanism under test: sie.STIE is the S-mode per-interrupt enable
// for the supervisor timer interrupt, independent of the global
// sstatus.SIE gate. A pended supervisor timer interrupt must stay
// pending without trapping while STIE=0 (even with SIE=1), and must
// trap exactly once STIE is set.
//
// Layout: src/boot.S jumps to main() in M-mode; the M-mode boot
// delegates the supervisor timer interrupt via mideleg bit 5, grants
// S-mode the cycle/time counters via mcounteren, opens the address
// space with one PMP NAPOT entry, installs the S-mode trap vector, and
// mret drops into s_main() in S-mode. The S-mode payload runs two
// phases:
//   Phase A (gate closed): sstatus.SIE is SET (the global gate open),
//     stimecmp = mtime + STG_AHEAD_TICKS is armed with sie.STIE CLEAR,
//     observe sip STIP go pending while a bounded rdcycle window runs,
//     and require zero traps.
//   Phase B (gate open): set STIE=1; exactly one trap must fire with
//     scause 0x8000000000000005; the handler disarms stimecmp to
//     all-ones; a bounded quiet window then requires no re-delivery.
//
// The two sibling modules share only src/boot.S and the UART driver via
// the Makefile; the boot, trap entry, handler, and reporting code here
// are written for this module alone.

#ifndef STG_H
#define STG_H

// Supervisor timer interrupt: scause interrupt bit set, code 5.
#define SCAUSE_STI 0x8000000000000005UL

// sip STIP bit (bit 5): set by the hart when mtime >= stimecmp,
// regardless of whether sie.STIE allows the trap to be taken.
#define SIP_STIP 0x20UL

// sie STIE bit (bit 5): the per-interrupt enable under test; kept clear
// through phase A so STIE alone is the gate.
#define SIE_STIE 0x20UL

// sstatus.SIE is bit 1: the global gate, open for the whole run.
#define SSTATUS_SIE 0x2UL

// Arming: stimecmp = mtime + 5000 mtime ticks (500 us of virtual time).
#define STG_AHEAD_TICKS 5000UL

// Bounded observation windows, in rdcycle reads.
#define STG_GATED_READS 100000UL
#define STG_QUIET_READS 100000UL

// Bounded wait for the one trap, in spin-loop iterations (never a hang).
#define STG_TRAP_WAIT_READS 10000000UL

// stimecmp (Sstc) CSR number; menvcfg bit 63 (STCE) enables S-mode access.
#define CSR_STIMECMP 0x14d
#define MENVCFG_STCE (1UL << 63)

// mideleg bit 5 (STI): delegate the supervisor timer interrupt to S-mode.
#define MIDELEG_STI (1UL << 5)

// CLINT mtimecmp for hart 0 on the QEMU virt board (M-mode only).
#define CLINT_MTIMECMP 0x02004000UL

// Trap save area. Offsets must match stg_trap.S:
//   t0_save@0 t1_save@8 scause@16 sepc@24,
//   then ra,sp,gp,tp,t2,s0,s1,a0-a7,s2-s11,t3-t6 at 32,40,...,256
//   (gpr[29]; the last register lands exactly at the struct end).
typedef struct {
    unsigned long t0_save;
    unsigned long t1_save;
    unsigned long scause;
    unsigned long sepc;
    unsigned long gpr[29];
} stg_save_t;

extern stg_save_t stg_save;
extern volatile unsigned long stg_trap_count;
extern volatile unsigned long stg_scause;       // the one trap's scause
extern volatile unsigned long stg_sepc;         // the one trap's sepc
extern volatile unsigned long stg_stip_gated;   // sip STIP seen while gated
extern volatile unsigned long stg_bad_seen;     // any trap beyond the first
extern volatile unsigned long stg_bad_scause;
extern volatile unsigned long stg_bad_sepc;
extern volatile unsigned long stg_stce;         // menvcfg.STCE probe result
extern volatile unsigned long stg_mideleg;      // mideleg readback

// Labels bounding the phase-B wait loop inside the asm block
// (stg_main.c): the one gate-open trap can only land between them.
extern char stg_loop[];
extern char stg_done[];

void stg_trap_entry(void);
void stg_trap_handler(stg_save_t *s);

// S-mode entry point: M-mode boot finishes with mret to s_main.
void s_main(void);

#endif // STG_H
