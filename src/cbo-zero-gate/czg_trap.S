    // czg_trap.S: trap entries for the cbo-zero-gate module.
    //
    // m_trap_entry (M-mode): mscratch points at the 12-word m_regs
    // array. Bumps the trap counter and records mcause/mepc/mstatus/
    // mtval on entry.
    //   - mcause == 9 (S-mode ecall): loads the continuation from
    //     slot 7 into mepc, sets mstatus.MPP to M-mode, and mret's
    //     into it. A zero continuation means a trap nobody planned
    //     for: park the hart.
    //   - any other M-mode trap (only the phase-0 probe cbo.zero can
    //     trap here, and only if the hart lacks Zicboz): records it,
    //     advances mepc by 4 past the faulting instruction, and
    //     mret's to resume, so the probe reports the trap honestly
    //     instead of hanging.
    //
    // s_trap_entry (S-mode): sscratch points at the 12-word s_regs
    // array. Bumps the trap counter, records scause/sepc/sstatus/
    // stval, advances sepc by 4 (cbo.zero is a 4-byte SYSTEM
    // instruction, never compressed), and sret's back into the
    // S-mode payload.
    //
    // Slot map (both arrays): 0 trap count, 1 parked t1, 2 cause,
    // 3 pc, 4 status-at-entry, 5 spare, 6 parked t2, 7 (M-mode)
    // continuation, 8 trap value (mtval/stval), 9-11 spare.

    .section .text
    .option push
    .option norvc
    .global m_trap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
m_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &m_regs, interrupted t0 parked in mscratch
    sd t1, 1*8(t0)           // park interrupted t1 in slot 1
    sd t2, 6*8(t0)           // park interrupted t2 in slot 6
    ld t1, 0*8(t0)           // bump the trap counter in slot 0
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, mcause
    sd t1, 2*8(t0)           // slot 2: mcause of this trap
    csrr t1, mepc
    sd t1, 3*8(t0)           // slot 3: mepc of this trap
    csrr t1, mstatus
    sd t1, 4*8(t0)           // slot 4: mstatus at handler entry
    csrr t1, mtval
    sd t1, 8*8(t0)           // slot 8: mtval of this trap
    ld t1, 2*8(t0)           // reload mcause
    li t2, 9                 // S-mode environment call?
    bne t1, t2, m_unexpected
    ld t1, 7*8(t0)           // slot 7: continuation
    beqz t1, m_park          // zero: unplanned trap, park the hart
    csrw mepc, t1            // resume at the continuation
    li t1, (3 << 11)         // MPP = M-mode: mret takes the privilege
    csrr t2, mstatus         //   from MPP, and the trap arrived from
    or t2, t2, t1            //   S-mode (MPP = 01), so without this the
    csrw mstatus, t2         //   mret would drop back into S-mode
    j m_restore
m_unexpected:
    csrr t1, mepc
    addi t1, t1, 4            // skip the faulting instruction, resume
    csrw mepc, t1
m_restore:
    ld t2, 6*8(t0)           // restore the interrupted t2
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, mscratch, t0   // restore t0, park &m_regs back in mscratch
    mret
m_park:
    wfi
    j m_park
    .option pop

    .option push
    .option norvc
    .global s_trap_entry
    .align 2                      // 4-byte aligned: stvec MODE=00 needs base[1:0] clear
s_trap_entry:
    csrrw t0, sscratch, t0   // t0 = &s_regs, interrupted t0 parked in sscratch
    sd t1, 1*8(t0)           // park interrupted t1 in slot 1
    sd t2, 6*8(t0)           // park interrupted t2 in slot 6
    ld t1, 0*8(t0)           // bump the trap counter in slot 0
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, scause
    sd t1, 2*8(t0)           // slot 2: scause of this trap
    csrr t1, sepc
    sd t1, 3*8(t0)           // slot 3: sepc of this trap
    addi t1, t1, 4
    csrw sepc, t1            // resume after the faulting instruction
    csrr t1, sstatus
    sd t1, 4*8(t0)           // slot 4: sstatus at handler entry
    csrr t1, stval
    sd t1, 8*8(t0)           // slot 8: stval of this trap
    ld t2, 6*8(t0)           // restore the interrupted t2
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, sscratch, t0   // restore t0, park &s_regs back in sscratch
    sret
    .option pop
