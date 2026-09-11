// st_trap.S: S-mode trap entry for the scounteren-trap module.
//
// sscratch points at the 6-word st_regs array in st_main.c. On
// entry the handler swaps t0 with sscratch, records scause, stval,
// and the sepc value the hardware saved at trap entry
// (st_regs[0..2]), bumps the trap counter in st_regs[3], then
// advances sepc by 4 and records the adjusted value in st_regs[4].
// The +4 advance is exact here: the only faulting instruction the
// module executes is rdcycle, which has no compressed encoding, so
// the faulting instruction is always 4 bytes. sret resumes at the
// labeled resume address and the fault is skipped exactly once
// instead of re-executing. st_regs[5] holds the interrupted t0.
//
// Only t0 (scratch-swapped) and t1 (left clobbered, documented)
// are touched.

    .section .text
    .option push
    .option norvc
    .global st_trap_entry
    .align 2                      // 4-byte aligned: stvec MODE=00 needs base[1:0] clear
st_trap_entry:
    csrrw t0, sscratch, t0   // t0 = &st_regs, interrupted t0 parked in sscratch
    csrr t1, scause
    sd t1, 0*8(t0)
    csrr t1, stval
    sd t1, 1*8(t0)
    csrr t1, sepc
    sd t1, 2*8(t0)          // sepc as saved by the hardware at trap entry
    ld t1, 3*8(t0)
    addi t1, t1, 1
    sd t1, 3*8(t0)          // trap_count++
    csrr t1, sepc
    addi t1, t1, 4
    sd t1, 4*8(t0)          // sepc after the handler's +4 adjust
    csrw sepc, t1           // resume at faulting instruction + 4
    csrr t1, sscratch
    sd t1, 5*8(t0)          // interrupted t0
    csrrw t0, sscratch, t0   // symmetric swap: interrupted t0 back
    sret
    .option pop
