// mtie_trap.S: M-mode trap entry for the mie.MTIE timer-interrupt
// enable-gate module.
//
// mscratch points at the 8-word mtie_regs array in mtie_main.c.
// On entry the handler saves t0 through mscratch, then records
// mcause, mepc, mtval, and the rdcycle stamp at trap entry, and
// calls the C handler mtie_c_handle(), which disarms the CLINT
// timer by writing mtimecmp to all-ones (the compare then sits
// above the running mtime, which clears MTIP and is what stops the
// re-fire) and bumps the trap counter. Everything the interrupted
// code needs is restored before mret; ra/a0 are saved because the
// C call clobbers them. The interrupted code in this module only
// spins on volatile counters, so the minimal save set is
// sufficient.

    .section .text
    .global mtie_trap_entry
    .align 2
mtie_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &mtie_regs, interrupted t0 parked in mscratch
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
    call mtie_c_handle       // disarms the timer, counts the trap, records mcause
    ld ra, 6*8(t0)
    ld a0, 7*8(t0)
    ld t1, 1*8(t0)           // restore the interrupted t1
    // One swap restores the interrupted t0 and parks &mtie_regs back
    // in mscratch for the next trap.
    csrrw t0, mscratch, t0
    mret
