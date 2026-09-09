// sv39_trap.S: M-mode trap entry for the Sv39 page-table walk module.
//
// mscratch points at the 8-word sv39_save array in sv39_main.c. On entry
// the handler swaps t0 with mscratch, records mcause/mepc/mtval and the
// current stval (these traps are taken in M-mode, so hardware writes
// mtval only; stval is captured to show it is untouched), sets mepc to
// the resume address the S-mode test stored, flags the trap as seen,
// restores t0/t1, and returns with mret. Interrupts are never enabled,
// so every trap here is a synchronous fault from the S-mode test phase.
// The handler runs in M-mode with translation off, so its stores to
// sv39_save are plain physical accesses.

    .section .text
    .global sv39_trap_entry
    .align 2
sv39_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &sv39_save, old t0 parked in mscratch
    sd t1, 1*8(t0)           // save[1]: t1
    csrr t1, mcause
    sd t1, 2*8(t0)           // save[2]: mcause
    csrr t1, mepc
    sd t1, 3*8(t0)           // save[3]: mepc
    csrr t1, mtval
    sd t1, 4*8(t0)           // save[4]: mtval
    csrr t1, stval
    sd t1, 7*8(t0)           // save[7]: stval (untouched by M-mode traps)
    ld t1, 5*8(t0)           // save[5]: resume pc, stored before the access
    csrw mepc, t1
    li t1, 1
    sd t1, 6*8(t0)           // save[6]: seen flag
    ld t1, 1*8(t0)           // restore t1
    // Restore the interrupted t0 and put &sv39_save back into mscratch
    // in one swap (a plain csrr would leave mscratch clobbered).
    csrrw t0, mscratch, t0
    mret
