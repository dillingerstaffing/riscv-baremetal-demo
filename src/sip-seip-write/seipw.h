// seipw.h: shared definitions for the S-mode sip.SEIP read-only probe
// (backlog item: riscv sip-seip-write).
//
// Mechanism under test: bit 9 of sip (SEIP, the supervisor external
// interrupt pending bit) is read-only for S-mode. It reports the
// pending state of the supervisor external interrupt, which only
// M-mode (through mip) or the interrupt controller can change; an
// S-mode write of all ones to sip must leave SEIP exactly as it was.
// The module proves the write really executed by requiring the
// software-writable SSIP bit (bit 1) to read back set, and proves no
// trap was involved by keeping every trap in M-mode (mideleg stays
// zero) behind a counting M-mode handler whose count must stay zero.
//
// Layout: src/boot.S jumps to main() in M-mode; M-mode boot disarms
// the machine timer comparator, probes Sstc via menvcfg.STCE, opens
// the address space with one PMP NAPOT entry, grants the counters via
// mcounteren, delegates only the supervisor software interrupt
// (mideleg bit 1) so the S-mode SSIP write is observable, installs
// the direct-mode M-mode trap vector and mscratch, and mret drops
// into s_main() in S-mode. SEI (bit 9) is deliberately not delegated,
// so the SEIP bit under test keeps its M-mode trap path.
//   1. Records the sip readback before the write; SEIP must be 0 (no
//      UART/PLIC interrupt is asserted on the quiet board; if it is
//      set, the premise is aborted, not forced).
//   2. Writes all ones to sip and reads back: SEIP must be unchanged
//      (the write is ignored on the read-only bit) while SSIP reads
//      back set, proving the write executed. STIP behavior is
//      measured and reported as observed.
//   3. The M-mode trap count must stay 0 across the writes.
//   4. Writes zero to sip and reads back: SSIP/STIP clear, SEIP still
//      unchanged, with the exact legalized value asserted after both
//      writes.
//
// The module shares only src/boot.S and the UART driver with the
// other demos; the boot, trap entry, and reporting code are written
// for this module alone.

#ifndef SEIPW_H
#define SEIPW_H

// sip bits under test.
#define SIP_SEIP 0x200UL // bit 9: supervisor external interrupt pending, read-only for S-mode
#define SIP_SSIP 0x2UL   // bit 1: supervisor software interrupt pending, software-writable
#define SIP_STIP 0x20UL  // bit 5: supervisor timer interrupt pending

// sstatus.SIE is bit 1.
#define SSTATUS_SIE 0x2UL

// menvcfg bit 63 (STCE): exists only when the Sstc extension is implemented.
#define MENVCFG_STCE (1UL << 63)

// mideleg bits: only SSI (bit 1) is delegated, so the S-mode SSIP
// write is observable. STI (bit 5) and SEI (bit 9) stay clear, so
// timer and external traps stay in M-mode.
#define MIDELEG_SSI (1UL << 1)
#define MIDELEG_STI (1UL << 5)
#define MIDELEG_SEI (1UL << 9)

// CLINT mtimecmp for hart 0 on the QEMU virt board (M-mode only).
#define CLINT_MTIMECMP 0x02004000UL

// M-mode trap save area. Offsets must match seipw_trap.S:
//   t0_save@0 t1_save@8 count@16 mcause@24 mepc@32 t2_save@40 t3_save@48.
#define SEIPW_COUNT_OFF 16
#define SEIPW_MCAUSE_OFF 24
#define SEIPW_MEPC_OFF 32

void m_trap_entry(void);

// S-mode entry point: M-mode boot finishes with mret to s_main.
void s_main(void);

#endif // SEIPW_H
