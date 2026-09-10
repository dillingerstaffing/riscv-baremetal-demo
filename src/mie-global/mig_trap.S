// mig_trap.S: M-mode trap entry for the mstatus.MIE global-gate module.
//
// mscratch points at the 9-word mig_regs array in mig_main.c.
// On entry the handler saves t0 through mscratch, then records
// mcause, mepc, mtval, the rdcycle stamp, and the mstatus word as
// seen on trap entry (MIE auto-cleared by hardware, MPIE/MPP set),
// and calls the C handler mig_c_handle(), which clears the CLINT
// msip register and bumps the trap counter. Everything the
// interrupted code needs is restored before mret; ra/a0 are saved
// because the C call clobbers them.

    .section .text
    .global mig_trap_entry
    .align 2
mig_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &mig_regs, interrupted t0 parked in mscratch
    sd t1, 1*8(t0)
    csrr t1, mcause
    sd t1, 2*8(t0)
    csrr t1, mepc
    sd t1, 3*8(t0)
    csrr t1, mtval
    sd t1, 4*8(t0)
    csrr t1, cycle
    sd t1, 5*8(t0)           // entry timestamp, kept with the record
    csrr t1, mstatus
    sd t1, 8*8(t0)           // mstatus as seen on trap entry
    sd ra, 6*8(t0)
    sd a0, 7*8(t0)
    call mig_c_handle        // clears msip, counts the trap, records values
    ld ra, 6*8(t0)
    ld a0, 7*8(t0)
    ld t1, 1*8(t0)           // restore the interrupted t1
    // One swap restores the interrupted t0 and parks &mig_regs back
    // in mscratch for the next trap.
    csrrw t0, mscratch, t0
    mret
