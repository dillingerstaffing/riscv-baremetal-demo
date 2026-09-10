// mrs_trap.S: M-mode trap entry for the mepc-resume-skip module.
//
// mscratch points at the 6-word mrs_regs array in mrs_main.c. On entry
// the handler swaps t0 with mscratch, records mcause, mtval, and the
// mepc value the hardware saved at trap entry (mrs_regs[0..2]), bumps
// the trap counter in mrs_regs[3], then advances mepc by 4 and records
// the adjusted value in mrs_regs[4]. The mepc advance is the mechanism
// under test: the faulting instruction is exactly 4 bytes (the module
// assembles the fault site with .option norvc), so mret resumes at the
// instruction immediately after the fault, and the fault is skipped
// exactly once instead of re-executing. mrs_regs[5] is unused, kept so
// the array stays a round 48 bytes.
//
// Only t0 (scratch-swapped) and t1 (left clobbered, documented) are
// touched. a0/a1, which carry the pre-fault sentinel and the faulting
// address, are not modified by the handler, so the post-resume code
// can verify the faulting load never committed.

    .section .text
    .option push
    .option norvc
    .global mrs_trap_entry
    .align 2
mrs_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &mrs_regs, interrupted t0 parked in mscratch
    csrr t1, mcause
    sd t1, 0*8(t0)
    csrr t1, mtval
    sd t1, 1*8(t0)
    csrr t1, mepc
    sd t1, 2*8(t0)          // mepc as saved by the hardware at trap entry
    ld t1, 3*8(t0)
    addi t1, t1, 1
    sd t1, 3*8(t0)          // trap_count++
    csrr t1, mepc
    addi t1, t1, 4
    sd t1, 4*8(t0)          // mepc after the handler's +4 adjust
    csrw mepc, t1           // resume at faulting instruction + 4
    csrrw t0, mscratch, t0   // symmetric swap: interrupted t0 back
    mret
    .option pop
