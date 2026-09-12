// mipf_trap.S: trap entries for the medeleg instruction-page-fault
// routing module.
//
// m_trap_entry (M-mode): mscratch points at the 10-word m_regs array.
// Bumps the trap counter and records mcause/mepc/mstatus/mtval at
// entry. The only M-mode traps this run can take are the phase-A
// S-mode ecall (medeleg bit 9 is clear, so it is not delegated) and
// the phase-B instruction page fault (medeleg bit 12 is clear in
// phase B), so the handler loads the continuation from slot 7
// (phase2_mmode, then report_mmode, placed by the C setup) into
// mepc, sets mstatus.MPP to M-mode, and mret's into it. A zero
// continuation means a trap nobody planned for: park the hart so the
// harness sees a timeout instead of a silent wrong resume.
//
// s_trap_entry (S-mode): sscratch points at the 10-word s_regs array.
// Bumps the trap counter, records scause/sepc/sstatus/stval at
// entry, loads the resume address the S-mode payload stored in slot
// 7 into sepc, and sret's back into the payload. Only the phase-A
// instruction page fault can trap here (medeleg bit 12 is set in
// phase A), and it arrives exactly once because the payload issues
// an ecall right after resuming.
//
// Slot map (both arrays): 0 trap count, 1 parked t1, 2 cause, 3 pc,
// 4 status-at-entry, 5 spare, 6 parked t2, 7 continuation (M-mode)
// or resume sepc (S-mode), 8 trap value (mtval/stval), 9 spare.

    .section .text
    .global m_trap_entry
    .align 2
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
    ld t1, 7*8(t0)           // slot 7: continuation
    beqz t1, mipf_m_park     // zero: unplanned trap, park
    csrw mepc, t1            // resume at the continuation
    li t1, (3 << 11)         // MPP = M-mode: mret takes the privilege
    csrr t2, mstatus         //   from MPP, and the trap arrived from
    or t2, t2, t1            //   S-mode (MPP = 01), so without this the
    csrw mstatus, t2         //   mret would drop back into S-mode
    ld t2, 6*8(t0)           // restore the interrupted t2
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, mscratch, t0   // restore t0, park &m_regs back in mscratch
    mret
mipf_m_park:
    wfi
    j mipf_m_park

    .global s_trap_entry
    .align 2
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
    sd t1, 3*8(t0)           // slot 3: sepc of this trap (faulting fetch)
    csrr t1, sstatus
    sd t1, 4*8(t0)           // slot 4: sstatus at handler entry
    csrr t1, stval
    sd t1, 8*8(t0)           // slot 8: stval of this trap
    ld t1, 7*8(t0)           // slot 7: resume address from the payload
    beqz t1, mipf_s_park     // zero: nothing to resume to, park
    csrw sepc, t1            // resume where the payload asked
    ld t2, 6*8(t0)           // restore the interrupted t2
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, sscratch, t0   // restore t0, park &s_regs back in sscratch
    sret
mipf_s_park:
    wfi
    j mipf_s_park
