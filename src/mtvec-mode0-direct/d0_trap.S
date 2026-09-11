// d0_trap.S: single M-mode trap entry for the mtvec direct-mode
// module (backlog item 138).
//
// With mtvec MODE=0 every trap, synchronous or asynchronous, enters
// at the single BASE address: this one entry point. The handler is
// deliberately minimal and there is exactly one trap entry in the
// whole image, so any trap that lands anywhere else would have to
// jump through memory no handler occupies; the C code cross-checks
// the recorded landing against the mtvec BASE readback.
//
// The handler records, per trap:
//   [0] landing: the link-time address of this entry (la-resolved by
//       the assembler), i.e. where the trap actually entered
//   [1] mcause
//   [2] mepc at entry
// then saves the caller-saved registers the C call may clobber,
// calls d0_c_handle(), and mret at the resume address the C handler
// chose (slot [18]).
//
// mscratch points at the d0_regs array in d0_main.c. The entry swaps
// t0 through mscratch, so the interrupted t0 is preserved across the
// whole trap.

    .section .text
    .align 2
    .global d0_trap_entry
d0_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &d0_regs, interrupted t0 parked
    sd t1, 4*8(t0)           // park interrupted t1 before using it
    la t1, d0_trap_entry
    sd t1, 0*8(t0)           // landing address observed by this trap
    csrr t1, mcause
    sd t1, 1*8(t0)
    csrr t1, mepc
    sd t1, 2*8(t0)
    sd ra, 3*8(t0)
    sd t2, 5*8(t0)
    sd t3, 6*8(t0)
    sd t4, 7*8(t0)
    sd t5, 8*8(t0)
    sd t6, 9*8(t0)
    sd a0, 10*8(t0)
    sd a1, 11*8(t0)
    sd a2, 12*8(t0)
    sd a3, 13*8(t0)
    sd a4, 14*8(t0)
    sd a5, 15*8(t0)
    sd a6, 16*8(t0)
    sd a7, 17*8(t0)
    // Park &d0_regs in mscratch across the C call: t0 is
    // caller-saved and cannot be trusted after it.
    csrrw t0, mscratch, t0
    call d0_c_handle
    csrrw t0, mscratch, t0   // t0 = &d0_regs again
    ld ra, 3*8(t0)
    ld t2, 5*8(t0)
    ld t3, 6*8(t0)
    ld t4, 7*8(t0)
    ld t5, 8*8(t0)
    ld t6, 9*8(t0)
    ld a0, 10*8(t0)
    ld a1, 11*8(t0)
    ld a2, 12*8(t0)
    ld a3, 13*8(t0)
    ld a4, 14*8(t0)
    ld a5, 15*8(t0)
    ld a6, 16*8(t0)
    ld a7, 17*8(t0)
    ld t1, 18*8(t0)      // resume mepc chosen by the C handler
    csrw mepc, t1
    ld t1, 4*8(t0)       // restore the interrupted t1
    csrrw t0, mscratch, t0 // restore the interrupted t0, park &d0_regs
    mret
