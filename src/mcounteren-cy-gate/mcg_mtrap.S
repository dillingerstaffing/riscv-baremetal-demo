// mcg_mtrap.S: M-mode trap entry for the mcounteren-cy-gate
// module.
//
// The experiment expects exactly one M-mode trap: an ecall from
// S-mode issued between phase A and phase B. On that ecall the
// handler records mcause/mepc and the trap count, sets
// mcounteren = 0x1 (CY only), records the readback, advances mepc
// past the ecall, and mret back to S-mode (mstatus.MPP is still
// 01, so the hart resumes in S-mode at the instruction after the
// ecall). Any other M-mode trap (mcause != 9) parks the hart; the
// bench harness runs QEMU under timeout, so an unexpected M-mode
// trap is observable as exit status 124 rather than a silent
// divergence.
//
// mscratch points at the 6-word mcg_m_regs array in mcg_main.c:
// [0] mcause, [1] mepc at entry, [2] M-mode trap count,
// [3] mcounteren readback after the set, [4] saved interrupted
// t1, [5] saved interrupted t2. Only t0 (scratch-swapped), t1,
// and t2 (both saved and restored) are touched.

    .section .text
    .option push
    .option norvc
    .global mcg_mtrap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
mcg_mtrap_entry:
    csrrw t0, mscratch, t0   // t0 = &mcg_m_regs, interrupted t0 parked in mscratch
    sd t1, 4*8(t0)          // save interrupted t1
    sd t2, 5*8(t0)          // save interrupted t2
    csrr t1, mcause
    li t2, 9                // ecall from S-mode
    bne t1, t2, mcg_mpark
    sd t1, 0*8(t0)          // mcause (== 9)
    csrr t1, mepc
    sd t1, 1*8(t0)          // mepc at the ecall
    ld t2, 2*8(t0)
    addi t2, t2, 1
    sd t2, 2*8(t0)          // M-mode trap count++
    li t2, 1                // CY only (QEMU resets mcounteren to 0,
    csrw mcounteren, t2     // so phase B must explicitly set it)
    csrr t2, mcounteren
    sd t2, 3*8(t0)          // mcounteren readback after the set
    csrr t2, mepc
    addi t2, t2, 4
    csrw mepc, t2           // resume at ecall + 4
    ld t1, 4*8(t0)          // restore interrupted t1
    ld t2, 5*8(t0)          // restore interrupted t2
    csrrw t0, mscratch, t0   // symmetric swap: interrupted t0 back
    mret
mcg_mpark:
    wfi
    j mcg_mpark
    .option pop
