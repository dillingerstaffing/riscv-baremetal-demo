// aaa_trap.S: M-mode trap entry for the amoadd.w experiment.
//
// mscratch points at the 4-word aaa_save array in aaa_main.c. On entry
// the handler swaps t0 with mscratch, increments the trap count,
// records mcause/mepc/mtval, restores t0 (putting &aaa_save back into
// mscratch), and parks the hart. No trap is expected in this module:
// amoadd.w on an aligned word, in M-mode, with no interrupts enabled,
// cannot fault here. If one ever fired, the run would stop in this
// handler without printing RESULT: PASS, and the recorded mcause/mepc/
// mtval would say exactly what happened. A printed PASS therefore
// implies the trap count stayed zero, and the C code checks the
// counter explicitly as well.

    .section .text
    .global aaa_trap_entry
    .align 2
aaa_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &aaa_save, old t0 parked in mscratch
    ld t1, 0(t0)
    addi t1, t1, 1
    sd t1, 0(t0)             // save[0]: trap count
    csrr t1, mcause
    sd t1, 8(t0)             // save[1]: mcause
    csrr t1, mepc
    sd t1, 16(t0)            // save[2]: mepc
    csrr t1, mtval
    sd t1, 24(t0)            // save[3]: mtval
    // Restore the interrupted t0 and put &aaa_save back into mscratch
    // in one swap: t0 still holds &aaa_save here, mscratch holds the
    // old t0. t1 is left clobbered; the hart never resumes.
    csrrw t0, mscratch, t0
halt_loop:
    wfi
    j halt_loop
