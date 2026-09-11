// mxl_trap.S: M-mode trap entry for the misa MXL WARL check.
//
// mscratch points at the 4-word mxl_regs array in mxl_main.c.
// On entry the handler saves the interrupted t0 through mscratch,
// records mcause, mepc, and mtval, bumps the trap count word, then
// restores the interrupted registers and returns with mret. The
// run installs this handler but never expects a trap: writing misa
// (even with an illegal MXL combination) is a legal M-mode CSR
// write, so any trap that fires is evidence the hart was disturbed
// and fails the run.

    .section .text
    .global mxl_trap_entry
    .align 2
mxl_trap_entry:
    csrrw t0, mscratch, t0  // t0 = &mxl_regs, interrupted t0 parked in mscratch
    csrr t1, mcause
    sd t1, 0*8(t0)
    csrr t1, mepc
    sd t1, 1*8(t0)
    csrr t1, mtval
    sd t1, 2*8(t0)
    ld t1, 3*8(t0)
    addi t1, t1, 1
    sd t1, 3*8(t0)
    csrrw t0, mscratch, t0  // restore interrupted t0, park &mxl_regs again
    mret
