// ss_trap.S: S-mode trap entry for the sepc-resume-skip module.
//
// sscratch points at the 6-word ss_regs array in ss_main.c. On entry
// the handler swaps t0 with sscratch, records scause, stval, and the
// sepc value the hardware saved at trap entry (ss_regs[0..2]), bumps
// the trap counter in ss_regs[3], then advances sepc by 4 and records
// the adjusted value in ss_regs[4]. The sepc advance is the mechanism
// under test: the faulting instruction is exactly 4 bytes (the module
// assembles the fault site with .option norvc), so sret resumes at the
// instruction immediately after the fault, and the fault is skipped
// exactly once instead of re-executing. ss_regs[5] is unused, kept so
// the array stays a round 48 bytes.
//
// Only t0 (scratch-swapped) and t1 (left clobbered, documented) are
// touched. a0/a1, which carry the pre-fault sentinel and the faulting
// address, are not modified by the handler, so the post-resume code
// can verify the faulting load never committed.
//
// ss_mtrap_park: M-mode trap entry, a safety net. No M-mode trap is
// expected in a correct run (the load fault is delegated to S-mode via
// medeleg); any M-mode trap parks the hart so the harness observes a
// timeout instead of executing from a wild vector.

    .section .text
    .option push
    .option norvc
    .global ss_trap_entry
    .align 2                      // 4-byte aligned: stvec MODE=00 needs base[1:0] clear
ss_trap_entry:
    csrrw t0, sscratch, t0   // t0 = &ss_regs, interrupted t0 parked in sscratch
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
    csrrw t0, sscratch, t0   // symmetric swap: interrupted t0 back
    sret

    .global ss_mtrap_park
    .align 2
ss_mtrap_park:
    wfi
    j ss_mtrap_park
    .option pop
