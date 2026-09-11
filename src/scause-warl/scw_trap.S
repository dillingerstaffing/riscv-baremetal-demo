// scw_trap.S: M-mode trap entry for the scause WARL module.
//
// mscratch points at the 8-word scw_regs array in scw_main.c.
// Words 0-2 hold the record (trap count, mcause, mepc); word 7
// is handler scratch for the interrupted t1.
//
// The only expected M-mode trap is the phase-complete signal:
// the S-mode payload issues a deliberate illegal instruction
// (.word 0), which medeleg leaves in M-mode with mcause = 2.
// The handler bumps the trap counter, records mcause/mepc,
// points mepc at scw_finish, sets mstatus.MPP to M-mode, and
// mret's. Any other mcause parks the hart in a wfi loop; the
// bench harness runs QEMU under timeout, so that path is a FAIL,
// observable as exit status 124.
//
// Only t0 (scratch-swapped) and t1/t2 (restored before mret) are
// touched; ra/a0 are never clobbered because the handler makes
// no C call.

    .section .text
    .option push
    .option norvc
    .global scw_trap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
scw_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &scw_regs, interrupted t0 parked in mscratch
    sd t1, 7*8(t0)           // park the interrupted t1 in word 7
    ld t1, 0*8(t0)           // bump the trap counter in slot 0
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, mcause
    sd t1, 1*8(t0)           // slot 1: mcause of this trap
    csrr t1, mepc
    sd t1, 2*8(t0)           // slot 2: mepc of this trap
    csrr t1, mcause
    li t2, 2                 // illegal instruction: the phase-complete signal
    bne t1, t2, scw_mpark
    // Phase-complete: resume in M-mode at scw_finish, not in the
    // trapped S-mode context.
    la t1, scw_finish
    csrw mepc, t1
    csrr t2, mstatus
    li t1, 0x1800            // clear MPP bits 12:11
    not t1, t1
    and t2, t2, t1
    li t1, 0x1800            // MPP = 11 (M-mode)
    or t2, t2, t1
    csrw mstatus, t2
    ld t1, 7*8(t0)           // restore the interrupted t1
    csrrw t0, mscratch, t0   // symmetric swap: interrupted t0 back
    mret
scw_mpark:
    wfi
    j scw_mpark
    .option pop
