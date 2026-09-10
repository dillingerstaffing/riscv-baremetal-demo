// sip_trap.S: M-mode trap entry for the sip SSIP bit write/readback
// module.
//
// mscratch points at the 4-word sip_regs array in sip_main.c.
// On entry the handler saves the interrupted t0 through mscratch,
// records mcause, mepc, and mtval, bumps the trap count word, then
// restores the interrupted registers and returns with mret. Both
// interrupt enables stay clear for the whole run, so this handler
// is expected never to fire; if it does, main reports the recorded
// values and the verdict goes to FAIL.

    .section .text
    .global sip_trap_entry
    .align 2
sip_trap_entry:
    csrrw t0, mscratch, t0  // t0 = &sip_regs, interrupted t0 parked in mscratch
    csrr t1, mcause
    sd t1, 0*8(t0)
    csrr t1, mepc
    sd t1, 1*8(t0)
    csrr t1, mtval
    sd t1, 2*8(t0)
    ld t1, 3*8(t0)
    addi t1, t1, 1
    sd t1, 3*8(t0)
    csrrw t0, mscratch, t0  // restore interrupted t0, park &sip_regs again
    mret
