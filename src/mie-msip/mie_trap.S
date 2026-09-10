// mie_trap.S: M-mode trap entry for the mie.MSIE enable-bit gate
// module.
//
// mscratch points at the 8-word mie_regs array in mie_main.c.
// On entry the handler saves t0 through mscratch, then records
// mcause, mepc, mtval, and the rdcycle stamp at trap entry, and
// calls the C handler mie_c_handle(), which clears the msip
// register and bumps the trap counter. Everything the interrupted
// code needs is restored before mret; ra/a0 are saved because the
// C call clobbers them.

    .section .text
    .global mie_trap_entry
    .align 2
mie_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &mie_regs, interrupted t0 parked in mscratch
    sd t1, 1*8(t0)
    csrr t1, mcause
    sd t1, 2*8(t0)
    csrr t1, mepc
    sd t1, 3*8(t0)
    csrr t1, mtval
    sd t1, 4*8(t0)
    csrr t1, cycle
    sd t1, 5*8(t0)           // entry timestamp, kept with the record
    sd ra, 6*8(t0)
    sd a0, 7*8(t0)
    call mie_c_handle        // clears msip, counts the trap, records mcause
    ld ra, 6*8(t0)
    ld a0, 7*8(t0)
    ld t1, 1*8(t0)           // restore the interrupted t1
    // One swap restores the interrupted t0 and parks &mie_regs back
    // in mscratch for the next trap.
    csrrw t0, mscratch, t0
    mret
