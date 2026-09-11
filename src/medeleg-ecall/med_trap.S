// med_trap.S: M-mode trap entry for the medeleg-ecall module.
//
// mscratch points at the 8-word med_regs array in med_main.c.
// Words 0-6 hold the record (trap count, mcause, mepc, phase-2
// ecall mcause/mepc/count, phase-complete count); word 7 is
// handler scratch for the interrupted t1.
//
//   - mcause 9 (environment call from S-mode): expected only in
//     phase 2, when medeleg bit 9 is clear. The handler copies the
//     cause/address into med_regs[3..4], bumps the ecall counter in
//     med_regs[5], advances mepc by 4 (ecall has no compressed
//     encoding), and mret's back into the S-mode payload.
//   - mcause 2 (illegal instruction): the phase-complete signal the
//     S-mode payload issues deliberately (.word 0) to hand control
//     back to M-mode. The handler bumps the phase-complete counter
//     in med_regs[6], points mepc at med_phase2 (phase 1) or
//     med_finish (phase 2), sets mstatus.MPP to M-mode, and mret's.
//   - any other mcause: park the hart; the bench harness observes
//     this as a timeout.
//
// Only t0 (scratch-swapped) and t1/t2 (clobbered, restored) are
// touched.

    .section .text
    .option push
    .option norvc
    .global med_trap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
med_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &med_regs, interrupted t0 parked in mscratch
    sd t1, 7*8(t0)           // park interrupted t1 in word 7 (handler scratch)
    ld t1, 0*8(t0)           // bump the trap counter in slot 0
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, mcause
    sd t1, 1*8(t0)           // slot 1: mcause of this trap
    csrr t1, mepc
    sd t1, 2*8(t0)           // slot 2: mepc of this trap
    csrr t1, mcause
    li t2, 9                 // environment call from S-mode
    bne t1, t2, med_not9
    // mcause == 9: phase-2 S-mode ecall. Record it if we are in
    // phase 2, then skip the ecall and resume the S-mode payload.
    la t2, med_phase
    ld t2, 0(t2)
    li t1, 2
    bne t2, t1, med_skip9
    la t2, med_regs
    ld t1, 1*8(t2)
    sd t1, 3*8(t2)           // slot 3: mcause of the phase-2 ecall
    ld t1, 2*8(t2)
    sd t1, 4*8(t2)           // slot 4: mepc of the phase-2 ecall
    ld t1, 5*8(t2)
    addi t1, t1, 1
    sd t1, 5*8(t2)           // slot 5: phase-2 ecall count
med_skip9:
    csrr t1, mepc
    addi t1, t1, 4
    csrw mepc, t1
    ld t1, 7*8(t0)           // restore the interrupted t1
    csrrw t0, mscratch, t0
    mret
med_not9:
    li t2, 2                 // illegal instruction: phase-complete signal
    bne t1, t2, med_park
    la t2, med_regs
    ld t1, 6*8(t2)
    addi t1, t1, 1
    sd t1, 6*8(t2)           // slot 6: phase-complete count
    // Return to M-mode code, not to the trapped S-mode context.
    la t2, med_phase
    ld t2, 0(t2)
    li t1, 1
    bne t2, t1, med_to_finish
    la t1, med_phase2
    j med_set_mpp
med_to_finish:
    la t1, med_finish
med_set_mpp:
    csrw mepc, t1
    csrr t2, mstatus
    li t1, 0x1800            // clear MPP bits 12:11
    not t1, t1
    and t2, t2, t1
    li t1, 0x1800            // MPP = 11 (M-mode)
    or t2, t2, t1
    csrw mstatus, t2
    ld t1, 7*8(t0)           // restore the interrupted t1
    csrrw t0, mscratch, t0
    mret
med_park:
    wfi
    j med_park
    .option pop
