// strm.h: shared definitions for the sip.STIP vs mideleg bit 5
// reconciliation probe (backlog item: riscv sip-stip-mideleg-reconcile).
//
// Question under test: two sibling modules measured the S-mode
// software write to sip bit 5 (STIP) differently on the surface.
// This module probes the same S-mode all-ones sip write twice in one
// boot: Phase A with mideleg bit 5 (STI delegation) CLEAR, Phase B
// with mideleg bit 5 SET. It publishes the sip readback after the
// write and after a zero write in each configuration, plus the
// mideleg value in each configuration, and requires zero unexpected
// traps throughout.
//
// The mechanism, from the RISC-V privileged specification and the
// siblings' measurements: on this QEMU the S-mode sip write path
// only admits bits whose interrupt is delegated via mideleg (the
// write-executed proof in each phase is SSIP, bit 1, which is
// delegated in both). STIP's writability is measured, not assumed:
// the exact legalized readback is asserted per phase from the
// observed stuck flags, and the reconciliation check requires the
// STIP-stuck flag to be identical in both configurations.
//
// Layout: src/boot.S jumps to main() in M-mode; M-mode boot disarms
// the machine timer comparator, probes Sstc via menvcfg.STCE, opens
// the address space with one PMP NAPOT entry, grants the counters
// via mcounteren, sets mideleg to delegate SSI only (bit 5 clear),
// installs the direct-mode M-mode trap vector and mscratch, and
// mret drops into s_main() in S-mode. Phase A runs the all-ones and
// zero writes, then issues a deliberate ecall. The M-mode trap
// handler recognizes the deliberate ecall (trap count 1,
// mcause 9), sets mideleg bit 5, records the new mideleg readback,
// and mret resumes in S-mode, where Phase B repeats the writes with
// STI now delegated. sie and sstatus.SIE stay clear for the whole
// run, so no pending bit can be taken as an interrupt.
//
// The module shares only src/boot.S and the UART driver with the
// other demos; the trap entry, boot, and reporting code are written
// for this module alone.

#ifndef STRM_H
#define STRM_H

// sip bits under test.
#define SIP_SSIP 0x2UL   // bit 1: supervisor software interrupt pending, software-writable
#define SIP_STIP 0x20UL  // bit 5: supervisor timer interrupt pending

// sstatus.SIE is bit 1.
#define SSTATUS_SIE 0x2UL

// menvcfg bit 63 (STCE): exists only when the Sstc extension is implemented.
#define MENVCFG_STCE (1UL << 63)

// mideleg bits: SSI (bit 1) is delegated in both phases so the
// S-mode sip writes are observable; STI (bit 5) is clear in Phase A
// and set in Phase B; SEI (bit 9) stays clear in both, so external
// traps keep their M-mode path.
#define MIDELEG_SSI (1UL << 1)
#define MIDELEG_STI (1UL << 5)
#define MIDELEG_SEI (1UL << 9)

// CLINT mtimecmp for hart 0 on the QEMU virt board (M-mode only).
#define CLINT_MTIMECMP 0x02004000UL

// M-mode trap save area. Offsets must match strm_trap.S:
//   t0_save@0 t1_save@8 count@16 mcause@24 mepc@32 t2_save@40 t3_save@48.
#define STRM_COUNT_OFF 16
#define STRM_MCAUSE_OFF 24
#define STRM_MEPC_OFF 32

void m_trap_entry(void);

// S-mode entry point: M-mode boot finishes with mret to s_main.
void s_main(void);

#endif // STRM_H
