// mpp_trap.S: M-mode trap entry for the mstatus.MPP encoding
// measurement module.
//
// mscratch points at the 8-word mpp_regs array in mpp_main.c. On
// entry the handler swaps t0, records mcause/mepc/mtval/mstatus,
// then calls the C dispatcher mpp_handle(&mpp_regs), which logs
// the trap and returns the resume pc (phase_done). The handler
// writes that pc to mepc, sets mstatus.MPP to M-mode so mret
// returns to M-mode, and resumes.
//
// Only t0/t1 are disturbed and both are restored before mret;
// the payload is a single ecall, so no other register state needs
// to survive the trap.

    .section .text
    .global mpp_trap_entry
    .align 2
mpp_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &mpp_regs, interrupted t0 parked in mscratch
    sd t1, 1*8(t0)
    csrr t1, mcause
    sd t1, 2*8(t0)
    csrr t1, mepc
    sd t1, 3*8(t0)
    csrr t1, mtval
    sd t1, 4*8(t0)
    csrr t1, mstatus         // MPP here is the mode we trapped from
    sd t1, 5*8(t0)
    csrr t1, mscratch        // t1 = interrupted t0 (t1 itself already saved)
    sd t1, 0*8(t0)
    mv a0, t0
    call mpp_handle          // returns resume pc in a0
    mv t1, a0
    csrw mepc, t1
    li t1, (3 << 11)
    csrs mstatus, t1         // MPP = M: mret resumes the reporter in M-mode
    ld t1, 1*8(t0)           // restore the interrupted t1
    // One swap restores the interrupted t0 and parks &mpp_regs back
    // in mscratch for the next trap.
    csrrw t0, mscratch, t0
    mret
