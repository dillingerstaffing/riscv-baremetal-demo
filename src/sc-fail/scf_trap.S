// scf_trap.S: M-mode trap entry for the sc-without-reservation
// experiment.
//
// mscratch points at the 4-word scf_save array in scf_main.c. On
// entry the handler swaps t0 with mscratch, records mcause/mepc/mtval
// into the array, flags the trap as seen, restores t0 (putting
// &scf_save back into mscratch), and parks the hart. No trap is
// expected in this experiment: the whole point is that a store
// conditional with no live reservation fails with rd != 0 instead of
// trapping. If one ever fired, the run would stop here without
// printing RESULT: PASS, and the recorded mcause/mepc/mtval would say
// exactly what happened.

    .section .text
    .global scf_trap_entry
    .align 2
scf_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &scf_save, old t0 parked in mscratch
    csrr t1, mcause
    sd t1, 0(t0)             // save[0]: mcause
    csrr t1, mepc
    sd t1, 8(t0)             // save[1]: mepc
    csrr t1, mtval
    sd t1, 16(t0)            // save[2]: mtval
    li t1, 1
    sd t1, 24(t0)            // save[3]: seen flag
    // Restore the interrupted t0 and put &scf_save back into mscratch
    // in one swap: t0 still holds &scf_save here, mscratch holds the
    // old t0. t1 is left clobbered; the hart never resumes.
    csrrw t0, mscratch, t0
halt_loop:
    wfi
    j halt_loop
