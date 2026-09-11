// scw_strap.S: S-mode trap entry for the scause WARL module.
//
// sscratch points at the 7-word st_regs array in scw_main.c.
// On entry the handler records scause in st_regs[0] first, then
// requires the cause to be 9 (environment call from S-mode),
// the only S-mode trap this module ever issues. For cause 9 it
// records stval, the sepc value the hardware saved at trap
// entry, and sstatus.SPP, bumps the trap counter in st_regs[3],
// then advances sepc by 4 and records the adjusted value in
// st_regs[4]. The +4 advance is exact here: the only trapping
// instruction the module executes in S-mode is ecall, which has
// no compressed encoding, so the trapping instruction is always
// 4 bytes. sret resumes at the labeled resume address and the
// ecall is skipped exactly once instead of re-executing.
//
// For any other scause the hart parks in a wfi loop instead of
// returning; the bench harness runs QEMU under timeout, so an
// unexpected S-mode trap is a FAIL, observable as exit status
// 124. Only t0 (scratch-swapped) and t1/t2 (clobbered,
// documented) are touched.

    .section .text
    .option push
    .option norvc
    .global scw_strap_entry
    .align 2                      // 4-byte aligned: stvec MODE=00 needs base[1:0] clear
scw_strap_entry:
    csrrw t0, sscratch, t0   // t0 = &st_regs, interrupted t0 parked in sscratch
    csrr t1, scause
    sd t1, 0*8(t0)          // st_regs[0]: scause, recorded for every entry
    li t2, 9                // environment call from S-mode: the only expected trap
    bne t1, t2, scw_spark
    csrr t1, stval
    sd t1, 1*8(t0)          // st_regs[1]: stval
    csrr t1, sepc
    sd t1, 2*8(t0)          // st_regs[2]: sepc as saved by the hardware at trap entry
    csrr t1, sstatus
    srli t1, t1, 8
    andi t1, t1, 1
    sd t1, 6*8(t0)          // st_regs[6]: sstatus.SPP at trap entry
    ld t1, 3*8(t0)
    addi t1, t1, 1
    sd t1, 3*8(t0)          // st_regs[3]: trap_count++
    csrr t1, sepc
    addi t1, t1, 4
    sd t1, 4*8(t0)          // st_regs[4]: sepc after the handler's +4 adjust
    csrw sepc, t1           // resume at trapping instruction + 4
    csrr t1, sscratch
    sd t1, 5*8(t0)          // st_regs[5]: interrupted t0
    csrrw t0, sscratch, t0  // symmetric swap: interrupted t0 back
    sret
scw_spark:
    wfi
    j scw_spark
    .option pop
