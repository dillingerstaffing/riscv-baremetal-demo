    // sfo_trap.S: M-mode trap entry for the sstatus-fs-off-fp-trap
    // module.
    //
    // mscratch points at the 4-word sfo_trap array in sfo_main.c.
    // On entry the handler swaps t0 with mscratch, records mcause,
    // mepc, and mtval (sfo_trap[0..2]), bumps the trap counter in
    // sfo_trap[3], advances mepc by 4, and mrets. The +4 skip is
    // correct for this module: the only instruction that can
    // synchronously trap is the 4-byte fadd.d, and interrupts are
    // masked (mstatus.MIE clear, mie == 0 at boot). Only t0
    // (scratch-swapped) and t1 are touched; the FP registers the
    // module uses are not modified by the handler, so the
    // post-trap f0 readback proves the FPU executed.

    .section .text
    .option push
    .option norvc
    .global sfo_trap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
sfo_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &sfo_trap, interrupted t0 parked in mscratch
    csrr t1, mcause
    sd t1, 0*8(t0)
    csrr t1, mepc
    sd t1, 1*8(t0)
    csrr t1, mtval
    sd t1, 2*8(t0)
    ld t1, 3*8(t0)
    addi t1, t1, 1
    sd t1, 3*8(t0)          // trap_count++
    csrr t1, mepc
    addi t1, t1, 4
    csrw mepc, t1           // resume past the 4-byte faulting instruction
    csrrw t0, mscratch, t0   // symmetric swap: interrupted t0 back
    mret
    .option pop
