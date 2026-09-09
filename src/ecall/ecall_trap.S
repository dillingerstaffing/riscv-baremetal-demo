// ecall_trap.S: M-mode trap entry for the ecall ABI round-trip module.
//
// mscratch points at the 32-word ecall_regs array in ecall_main.c. On
// entry the handler saves every register x1-x31 (x0 is always zero),
// then calls the C dispatcher ecall_handle(&ecall_regs), which logs the
// trap registers and the received argument registers, computes the
// ecall result into the saved a0 slot, and returns the resume pc. The
// handler stashes that pc in the unused x0 slot, restores every
// register, and returns with mret.
//
// Register plumbing across the C call: the interrupted t0 travels in
// mscratch (swapped on entry and exit, so a second trap still works);
// the save-area base travels in s0, which the C ABI guarantees is
// callee-saved, so the dispatcher cannot disturb it.

    .section .text
    .global ecall_trap_entry
    .align 2
ecall_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &ecall_regs, interrupted t0 parked in mscratch
    sd x1, 1*8(t0)
    sd x2, 2*8(t0)
    sd x3, 3*8(t0)
    sd x4, 4*8(t0)
    // x5 (t0) handled below via mscratch
    sd x6, 6*8(t0)
    sd x7, 7*8(t0)
    sd x8, 8*8(t0)
    sd x9, 9*8(t0)
    sd x10, 10*8(t0)
    sd x11, 11*8(t0)
    sd x12, 12*8(t0)
    sd x13, 13*8(t0)
    sd x14, 14*8(t0)
    sd x15, 15*8(t0)
    sd x16, 16*8(t0)
    sd x17, 17*8(t0)
    sd x18, 18*8(t0)
    sd x19, 19*8(t0)
    sd x20, 20*8(t0)
    sd x21, 21*8(t0)
    sd x22, 22*8(t0)
    sd x23, 23*8(t0)
    sd x24, 24*8(t0)
    sd x25, 25*8(t0)
    sd x26, 26*8(t0)
    sd x27, 27*8(t0)
    sd x28, 28*8(t0)
    sd x29, 29*8(t0)
    sd x30, 30*8(t0)
    sd x31, 31*8(t0)
    csrr t1, mscratch        // t1 = interrupted t0 (t1 itself already saved)
    sd t1, 5*8(t0)
    mv s0, t0                // save-area base rides the C call in s0 (callee-saved)
    mv a0, t0
    call ecall_handle        // returns resume pc in a0
    mv t0, s0                // save-area base back
    sd a0, 0*8(t0)           // stash resume pc in the unused x0 slot
    ld x1, 1*8(t0)
    ld x2, 2*8(t0)
    ld x3, 3*8(t0)
    ld x4, 4*8(t0)
    // x5 (t0) restored last via mscratch
    ld x6, 6*8(t0)
    ld x7, 7*8(t0)
    ld x8, 8*8(t0)
    ld x9, 9*8(t0)
    ld x10, 10*8(t0)
    ld x11, 11*8(t0)
    ld x12, 12*8(t0)
    ld x13, 13*8(t0)
    ld x14, 14*8(t0)
    ld x15, 15*8(t0)
    ld x16, 16*8(t0)
    ld x17, 17*8(t0)
    ld x18, 18*8(t0)
    ld x19, 19*8(t0)
    ld x20, 20*8(t0)
    ld x21, 21*8(t0)
    ld x22, 22*8(t0)
    ld x23, 23*8(t0)
    ld x24, 24*8(t0)
    ld x25, 25*8(t0)
    ld x26, 26*8(t0)
    ld x27, 27*8(t0)
    ld x28, 28*8(t0)
    ld x29, 29*8(t0)
    ld x30, 30*8(t0)
    ld x31, 31*8(t0)
    ld t1, 0*8(t0)           // t1 = resume pc (reused as scratch)
    csrw mepc, t1
    ld t1, 6*8(t0)           // restore the interrupted t1 (was scratch above)
    // One swap restores the interrupted t0 and parks &ecall_regs back
    // in mscratch for the next trap.
    csrrw t0, mscratch, t0
    mret
