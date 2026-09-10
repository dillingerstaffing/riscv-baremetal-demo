// mcause_trap.S: M-mode trap entry for the mcause WARL module.
//
// mscratch points at the 8-word mca_regs array in mcause_main.c.
// On entry the handler saves t0 through mscratch, then records
// mcause and mepc, and calls the C handler mca_c_handle(), which
// bumps the trap counter, records the cause, and (for the
// deliberate ecall, mcause = 11) advances the mepc CSR past the
// 4-byte ecall so the run continues. For any other trap cause the
// C handler parks the hart instead of returning; that path is a
// FAIL, observable as the harness timeout. Everything the
// interrupted code needs is restored before mret; ra/a0 are saved
// because the C call clobbers them. The interrupted code in this
// module only spins on volatile counters, so the minimal save set
// is sufficient.

    .section .text
    .global mcause_trap_entry
    .align 2
mcause_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &mca_regs, interrupted t0 parked in mscratch
    sd t1, 1*8(t0)
    csrr t1, mcause
    sd t1, 2*8(t0)
    csrr t1, mepc
    sd t1, 3*8(t0)
    sd ra, 4*8(t0)
    sd a0, 5*8(t0)
    call mca_c_handle        // counts the trap, records the cause
    ld ra, 4*8(t0)
    ld a0, 5*8(t0)
    ld t1, 1*8(t0)           // restore the interrupted t1
    // One swap restores the interrupted t0 and parks &mca_regs back
    // in mscratch for the next trap.
    csrrw t0, mscratch, t0
    mret
