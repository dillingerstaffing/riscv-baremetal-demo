// mib_trap.S: M-mode trap entry for the mcause interrupt-bit module.
//
// Direct-mode mtvec. mscratch points at the 8-word mib_regs array in
// mib_main.c. On entry the handler swaps t0 through mscratch (t0 then
// points at the save area, the interrupted t0 is parked), saves t1,
// records mcause and mepc, saves ra/a0 across the C call, calls
// mib_c_handle(), restores everything, and mret. The C handler
// decides the resume mepc: ecall resumes past the 4-byte ecall, the
// timer interrupt and anything else resume where they fired.

    .section .text
    .global mib_trap_entry
    .align 2
mib_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &mib_regs, interrupted t0 parked in mscratch
    sd t1, 1*8(t0)
    csrr t1, mcause
    sd t1, 2*8(t0)
    csrr t1, mepc
    sd t1, 3*8(t0)
    sd ra, 4*8(t0)
    sd a0, 5*8(t0)
    call mib_c_handle
    ld ra, 4*8(t0)
    ld a0, 5*8(t0)
    ld t1, 1*8(t0)           // restore the interrupted t1
    // One swap restores the interrupted t0 and parks &mib_regs back
    // in mscratch for the next trap.
    csrrw t0, mscratch, t0
    mret
