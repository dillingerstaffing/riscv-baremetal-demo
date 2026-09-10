// misa_trap.S: M-mode trap entry for the misa read-only check.
//
// mscratch points at the 4-word misa_regs array in misa_main.c.
// On entry the handler saves the interrupted t0 through mscratch,
// records mcause, mepc, and mtval, bumps the trap count word, then
// restores the interrupted registers and returns with mret. The
// run installs this handler but never expects a trap: writing misa
// is a legal M-mode CSR write, so any trap that fires (here or
// elsewhere) is evidence the hart was disturbed and fails the run.

    .section .text
    .global misa_trap_entry
    .align 2
misa_trap_entry:
    csrrw t0, mscratch, t0  // t0 = &misa_regs, interrupted t0 parked in mscratch
    csrr t1, mcause
    sd t1, 0*8(t0)
    csrr t1, mepc
    sd t1, 1*8(t0)
    csrr t1, mtval
    sd t1, 2*8(t0)
    ld t1, 3*8(t0)
    addi t1, t1, 1
    sd t1, 3*8(t0)
    csrrw t0, mscratch, t0  // restore interrupted t0, park &misa_regs again
    mret
