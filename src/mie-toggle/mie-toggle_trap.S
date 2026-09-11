// mie-toggle_trap.S: M-mode trap entry for the mstatus.MIE gate module.
//
// mscratch points at the 8-word mie_regs array in mie-toggle_main.c.
// On entry the handler saves t0 via mscratch, records mcause and mepc,
// then calls the C handler mie_c_handle(), which disarms the CLINT
// timer (mtimecmp = all-ones, so the level-triggered source drops and
// the trap cannot re-fire), bumps the trap counter, and records
// mcause/mepc. Everything the interrupted code needs is restored
// before mret.

    .section .text
    .global mie_toggle_trap_entry
    .align 2
mie_toggle_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &mie_regs, interrupted t0 parked in mscratch
    sd t1, 1*8(t0)
    csrr t1, mcause
    sd t1, 2*8(t0)
    csrr t1, mepc
    sd t1, 3*8(t0)
    sd ra, 4*8(t0)
    sd a0, 5*8(t0)
    call mie_c_handle        // disarms mtimecmp, counts the trap
    ld ra, 4*8(t0)
    ld a0, 5*8(t0)
    ld t1, 1*8(t0)           // restore the interrupted t1
    // One swap restores the interrupted t0 and parks &mie_regs back
    // in mscratch for the next trap.
    csrrw t0, mscratch, t0
    mret
