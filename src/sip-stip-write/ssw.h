// ssw.h: shared definitions for the sip.STIP software-write probe
// (backlog item: riscv sip-stip-write).
//
// Mechanism under test: with the Sstc extension present, bit 5 of
// sip (STIP) is writable in S-mode, so software can pend its own
// supervisor timer interrupt: set STIP by writing sip, observe the
// readback stick, and the trap must fire with scause
// 0x8000000000000005. The handler clears STIP, which must drop the
// pending bit and produce no re-delivery.
//
// Layout: src/boot.S jumps to main() in M-mode; the M-mode boot
// probes Sstc via menvcfg.STCE, disarms both comparators, opens the
// address space with one PMP NAPOT entry, grants S-mode the
// cycle/time counters via mcounteren, delegates the supervisor timer
// interrupt via mideleg bit 5, installs the S-mode trap vector, and
// mret drops into s_main() in S-mode. The S-mode payload:
//   1. stimecmp is disarmed to all-ones first, so no real timer can
//      fire during the run; sie.STIE is enabled.
//   2. csrs sip, STIP; the readback must show STIP stuck (the honest
//      fork: if the write does not stick, the module ships the
//      measured WARL-ignore result instead of forcing the premise).
//   3. sstatus.SIE is set inside the labeled wait region; exactly one
//      trap must fire with scause 0x8000000000000005 and sepc between
//      ssw_loop and ssw_done. The handler clears STIP and records the
//      sip readback after the clear.
//   4. A bounded quiet window with SIE on requires zero re-delivery.
//
// The module shares only src/boot.S and the UART driver with the
// other demos; the boot, trap entry, handler, and reporting code are
// written for this module alone.

#ifndef SSW_H
#define SSW_H

// Supervisor timer interrupt: scause interrupt bit set, code 5.
#define SCAUSE_STI 0x8000000000000005UL

// sip STIP bit (bit 5): software-writable in S-mode when Sstc is
// present; setting it pends the supervisor timer interrupt.
#define SIP_STIP 0x20UL

// sie STIE bit (bit 5): the per-interrupt enable for the supervisor
// timer; kept on for the whole S-mode run.
#define SIE_STIE 0x20UL

// sstatus.SIE is bit 1.
#define SSTATUS_SIE 0x2UL

// Bounded quiet window after the expected trap, in rdcycle reads.
#define SSW_QUIET_READS 200000UL

// Bounded wait for the expected trap, in wait-loop iterations
// (never a hang).
#define SSW_TRAP_WAIT_READS 10000000UL

// stimecmp (Sstc) CSR number; menvcfg bit 63 (STCE) enables S-mode access.
#define CSR_STIMECMP 0x14d
#define MENVCFG_STCE (1UL << 63)

// mideleg bit 5 (STI): delegate the supervisor timer interrupt to S-mode.
#define MIDELEG_STI (1UL << 5)

// CLINT mtimecmp for hart 0 on the QEMU virt board (M-mode only).
#define CLINT_MTIMECMP 0x02004000UL

// Trap save area. Offsets must match ssw_trap.S:
//   t0_save@0 t1_save@8 scause@16 sepc@24,
//   then ra,sp,gp,tp,t2,s0,s1,a0-a7,s2-s11,t3-t6 at 48,56,...,272.
typedef struct {
    unsigned long t0_save;
    unsigned long t1_save;
    unsigned long scause;
    unsigned long sepc;
    unsigned long gpr[29];
} ssw_save_t;

extern ssw_save_t ssw_save;
extern volatile unsigned long ssw_trap_count;
extern volatile unsigned long ssw_scause;          // the one trap's scause
extern volatile unsigned long ssw_sepc;            // the one trap's sepc
extern volatile unsigned long ssw_sip_after_clear; // sip readback in handler
extern volatile unsigned long ssw_bad_seen;        // any trap beyond the first
extern volatile unsigned long ssw_bad_scause;
extern volatile unsigned long ssw_bad_sepc;
extern volatile unsigned long ssw_stce;            // menvcfg.STCE probe result
extern volatile unsigned long ssw_mideleg;         // mideleg readback

// Labels bounding the wait loop inside the asm block (ssw_main.c):
// the one expected trap can only land between them.
extern char ssw_loop[];
extern char ssw_done[];

void ssw_trap_entry(void);
void ssw_trap_handler(ssw_save_t *s);

// S-mode entry point: M-mode boot finishes with mret to s_main.
void s_main(void);

#endif // SSW_H
