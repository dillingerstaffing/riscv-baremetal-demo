// lrsc_trap.S: M-mode trap entry for the LR/SC attempt-histogram
// experiment.
//
// mscratch points at the 4-word lrsc_save array in lrsc_main.c. On
// entry the handler swaps t0 with mscratch, records mcause/mepc/mtval
// into the array, flags the trap as seen, restores t0 (putting
// &lrsc_save back into mscratch), and parks the hart. No trap is
// expected in this experiment: a single hart issuing aligned lr.w/sc.w
// pairs with nothing else running. If one ever fired, the run would
// stop here without printing RESULT: PASS, and the recorded
// mcause/mepc/mtval would say exactly what happened.

    .section .text
    .global lrsc_trap_entry
    .align 2
lrsc_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &lrsc_save, old t0 parked in mscratch
    csrr t1, mcause
    sd t1, 0(t0)             // save[0]: mcause
    csrr t1, mepc
    sd t1, 8(t0)             // save[1]: mepc
    csrr t1, mtval
    sd t1, 16(t0)            // save[2]: mtval
    li t1, 1
    sd t1, 24(t0)            // save[3]: seen flag
    // Restore the interrupted t0 and put &lrsc_save back into mscratch
    // in one swap: t0 still holds &lrsc_save here, mscratch holds the
    // old t0. t1 is left clobbered; the hart never resumes.
    csrrw t0, mscratch, t0
halt_loop:
    wfi
    j halt_loop
