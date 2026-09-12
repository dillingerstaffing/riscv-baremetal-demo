// fru_trap.S: counting M-mode trap entry, safety net for the
// fcsr-frm-roundup module.
//
// mscratch points at the 4-word fru_trap array in fru_main.c. On
// entry the handler swaps t0 with mscratch, records mcause, mepc,
// and mtval (fru_trap[0..2]), bumps the trap counter in
// fru_trap[3], advances mepc by 4, and mrets. The +4 skip is
// correct for this module: every instruction that can
// synchronously trap (fadd.d, fdiv.d, csrw) is 4 bytes, and
// interrupts are masked (mstatus.MIE clear, mie == 0 at boot).
// The module requires the counter to stay 0; an unexpected trap
// still completes the run with RESULT: FAIL and the trap details
// printed, instead of hanging the harness.
//
// Only t0 (scratch-swapped) and t1 are touched; the FP registers
// the module writes are not modified by the handler.

    .section .text
    .option push
    .option norvc
    .global fru_trap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
fru_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &fru_trap, interrupted t0 parked in mscratch
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
