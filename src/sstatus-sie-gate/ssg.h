// ssg.h: shared definitions for the sstatus.SIE gate experiment
// (proof backlog item 176).
//
// One mechanism under test: sstatus.SIE gates supervisor-mode interrupt
// delivery independently of the pending bits. A pended supervisor timer
// interrupt must stay pending without trapping while SIE=0, and must
// trap the moment SIE=1.
//
// Layout: src/boot.S jumps to main() in M-mode; the M-mode boot
// delegates the supervisor timer interrupt via mideleg bit 5, grants
// S-mode the cycle/time counters via mcounteren, opens the address
// space with one PMP NAPOT entry, installs the S-mode trap vector, and
// mret drops into s_main() in S-mode. The S-mode payload runs two
// phases:
//   Phase A (gate closed): arm stimecmp = mtime + SSG_AHEAD_TICKS with
//     sstatus.SIE clear, observe sip STIP go pending while a bounded
//     rdcycle window runs, and require zero traps.
//   Phase B (gate open): set SIE=1; exactly one trap must fire with
//     scause 0x8000000000000005; the handler disarms stimecmp to
//     all-ones; a bounded quiet window then requires no re-delivery.
//
// The two modules share only src/boot.S and the UART driver via the
// Makefile; the boot, trap entry, handler, and reporting code here
// are written for this module alone.

#ifndef SSG_H
#define SSG_H

// Supervisor timer interrupt: scause interrupt bit set, code 5.
#define SCAUSE_STI 0x8000000000000005UL

// sip STIP bit (bit 5): set by the hart when mtime >= stimecmp,
// regardless of whether sstatus.SIE allows the trap to be taken.
#define SIP_STIP 0x20UL

// sie STIE bit (bit 5): the per-interrupt enable for the supervisor
// timer; kept on for the whole run so SIE alone is the gate.
#define SIE_STIE 0x20UL

// sstatus.SIE is bit 1.
#define SSTATUS_SIE 0x2UL

// Arming: stimecmp = mtime + 5000 mtime ticks (500 us of virtual time).
#define SSG_AHEAD_TICKS 5000UL

// Bounded observation windows, in rdcycle reads.
#define SSG_GATED_READS 200000UL
#define SSG_QUIET_READS 200000UL

// Bounded waits on the mtime clock, in mtime ticks (never a hang).
#define SSG_PENDING_WAIT_TICKS 1000000UL
#define SSG_TRAP_WAIT_READS    10000000UL

// stimecmp (Sstc) CSR number; menvcfg bit 63 (STCE) enables S-mode access.
#define CSR_STIMECMP 0x14d
#define MENVCFG_STCE (1UL << 63)

// mideleg bit 5 (STI): delegate the supervisor timer interrupt to S-mode.
#define MIDELEG_STI (1UL << 5)

// CLINT mtimecmp for hart 0 on the QEMU virt board (M-mode only).
#define CLINT_MTIMECMP 0x02004000UL

// Trap save area. Offsets must match ssg_trap.S:
//   t0_save@0 t1_save@8 scause@16 sepc@24,
//   then ra,sp,gp,tp,t2,s0,s1,a0-a7,s2-s11,t3-t6 at 48,56,...,272.
typedef struct {
    unsigned long t0_save;
    unsigned long t1_save;
    unsigned long scause;
    unsigned long sepc;
    unsigned long gpr[29];
} ssg_save_t;

extern ssg_save_t ssg_save;
extern volatile unsigned long ssg_trap_count;
extern volatile unsigned long ssg_scause;       // the one trap's scause
extern volatile unsigned long ssg_sepc;         // the one trap's sepc
extern volatile unsigned long ssg_stip_gated;   // sip STIP seen while gated
extern volatile unsigned long ssg_bad_seen;     // any trap beyond the first
extern volatile unsigned long ssg_bad_scause;
extern volatile unsigned long ssg_bad_sepc;
extern volatile unsigned long ssg_stce;         // menvcfg.STCE probe result
extern volatile unsigned long ssg_mideleg;     // mideleg readback

// Labels bounding the phase-B wait loop inside the asm block
// (ssg_main.c): the one gate-open trap can only land between them.
extern char ssg_loop[];
extern char ssg_done[];

void ssg_trap_entry(void);
void ssg_trap_handler(ssg_save_t *s);

// S-mode entry point: M-mode boot finishes with mret to s_main.
void s_main(void);

#endif // SSG_H
