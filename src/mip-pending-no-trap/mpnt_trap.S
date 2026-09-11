// mpnt_trap.S: M-mode trap entry for the mip pending-bit-without-trap
// module (backlog item 171).
//
// mscratch points at the 5-word mpnt_regs array in mpnt_main.c.
// On entry the handler parks the interrupted t1 in slot 4, bumps the
// trap counter in slot 0, records mcause/mepc/mtval in slots 1-3,
// advances mepc by 4 past the trapping instruction (every instruction
// in this module that could trap is 4 bytes: CSR accesses, loads and
// stores), restores the interrupted registers, and returns with mret.
// Both the per-interrupt enable (mie.MSIE) and the global gate
// (mstatus.MIE) stay clear for the whole run, so this handler is
// expected never to fire; if it does, main reports the recorded
// values and the verdict goes to FAIL. No labels-as-values are used
// anywhere, so the GCC label-miscompile concern does not arise.

    .section .text
    .global mpnt_trap_entry
    .align 2
mpnt_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &mpnt_regs, interrupted t0 parked in mscratch
    sd t1, 4*8(t0)           // park interrupted t1 in slot 4
    ld t1, 0*8(t0)           // bump the trap counter in slot 0
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, mcause
    sd t1, 1*8(t0)           // slot 1: mcause of this trap
    csrr t1, mepc
    sd t1, 2*8(t0)           // slot 2: mepc of this trap
    csrr t1, mtval
    sd t1, 3*8(t0)           // slot 3: mtval of this trap
    csrr t1, mepc            // skip the trapping instruction on resume
    addi t1, t1, 4
    csrw mepc, t1
    ld t1, 4*8(t0)           // restore the interrupted t1
    // One swap restores the interrupted t0 and parks &mpnt_regs back
    // in mscratch for the next trap.
    csrrw t0, mscratch, t0
    mret
