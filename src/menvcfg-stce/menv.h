// menv.h: shared definitions for the menvcfg STCE advertisement probe.
//
// The experiment: menvcfg bit 63 (STCE) advertises the Sstc extension to
// lower privilege levels. M-mode reads menvcfg, probes the bit's WARL
// behavior (clear it, write all-ones, restore), then drops to S-mode and
// checks whether real S-mode access to the stimecmp CSR agrees with the
// advertisement. If the advertisement is honest and Sstc is present, a
// supervisor timer interrupt is armed from S-mode and exactly one trap
// with scause = 0x8000000000000005 must fire, followed by a quiet window
// with zero re-delivery traps.
//
// Written independently of src/stimecmp-one-shot; the two modules share
// only boot.S and the UART driver via the Makefile.

#ifndef MENV_H
#define MENV_H

// menvcfg bit 63: STCE, S-mode access to the Sstc time-compare CSR.
#define MENVCFG_STCE (1UL << 63)

// stimecmp (Sstc) CSR number, accessed by number so the module does not
// depend on the toolchain's CSR tables.
#define CSR_STIMECMP 0x14d

// Supervisor timer interrupt: scause interrupt bit set, code 5.
#define SCAUSE_STI 0x8000000000000005UL

// sip STIP bit (bit 5): the pending bit the supervisor timer sets.
#define SIP_STIP 0x20UL

// mideleg bits delegated to S-mode: 2 (illegal instruction, so the
// phase-1 stimecmp access probe is observable in S-mode) and 5
// (supervisor timer interrupt).
#define MENV_MIDELEG (0x24UL)

// One-shot arming: stimecmp = mtime + MENV_AHEAD_TICKS.
#define MENV_AHEAD_TICKS 10000UL

// Quiet window length in rdcycle reads; must see zero re-delivery traps.
#define MENV_QUIET_READS 100000UL

// Trap save area. Offsets must match menv_trap.S:
//   t0_save@0 t1_save@8 scause@16 sepc@24 (original, before any +4),
//   then ra,sp,gp,tp,t2,s0,s1,a0-a7,s2-s11,t3-t6 at 32,40,...,256,
//   sepc_adj@264 (sepc after the entry-time adjust).
typedef struct {
    unsigned long t0_save;
    unsigned long t1_save;
    unsigned long scause;
    unsigned long sepc;
    unsigned long gpr[29];
    unsigned long sepc_adj;
} menv_save_t;

extern menv_save_t menv_save;
extern volatile unsigned long menv_trap_count; // S-mode traps so far
extern volatile unsigned long menv_flag;       // wakes the arming spin loop
extern volatile unsigned long menv_scause;     // first trap's scause
extern volatile unsigned long menv_sepc;       // first trap's sepc
extern volatile unsigned long menv_sip_after;  // sip read in the handler
extern volatile unsigned long menv_cmp_after;  // stimecmp read in handler
extern volatile unsigned long menv_bad_seen;   // any trap beyond the first
extern volatile unsigned long menv_bad_scause;
extern volatile unsigned long menv_bad_sepc;
extern volatile unsigned long menv_arm_cmp;    // the written stimecmp value
extern volatile unsigned long menv_arm_clean;  // 1 if mtime < cmp at write
extern volatile unsigned long menv_quiet_traps;
extern volatile unsigned long menv_s_writes;   // S-mode stimecmp writes
extern volatile unsigned long menv_quiet_done;
extern volatile unsigned long menv_mideleg_rb;

// Labels bounding the spin loop inside the arming asm block: the timer
// trap can only land between them while SIE is on.
extern char menv_loop[];
extern char menv_done[];

void menv_strap_entry(void);
void menv_mtrap_entry(void);
void menv_s_trap_handler(menv_save_t *s);
void menv_m_unexpected_trap(void);

// S-mode entry point: M-mode boot finishes with mret to s_main.
void s_main(void);

#endif // MENV_H
