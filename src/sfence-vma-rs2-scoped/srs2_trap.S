// srs2_trap.S: trap entries for the sfence.vma rs2-scoped
// (ASID-scoped) module.
//
// m_trap_entry (M-mode): mscratch points at the 10-word m_regs array.
// Bumps the trap counter and records mcause/mepc/mstatus/mtval at
// entry. The only M-mode traps this run can take are the five
// S-mode ecalls that return from the phases (medeleg is zero, so
// cause 9 is not delegated), so the handler loads the continuation
// from slot 7 (mid_to_B, mid_to_C, mid_to_D, mid_to_E, then
// report_mmode, placed by the C setup) into mepc, sets
// mstatus.MPP to M-mode, and mret's into it. A zero continuation
// means a trap nobody planned for: park the hart so the harness
// sees a timeout instead of a silent wrong resume.
//
// s_trap_entry (S-mode): sscratch points at the 10-word s_regs array.
// Bumps the trap counter and parks the hart. No S-mode trap is
// expected at any point in this run (the phases only load and
// ecall), so any arrival here is a hard failure and the parked
// hart makes it loud. A faulting phase load would land here, so a
// bad readback can never be mistaken for a measured value.
//
// Slot map (both arrays): 0 trap count, 1 parked t1, 2 cause, 3 pc,
// 4 status-at-entry, 5 spare, 6 parked t2, 7 continuation (M-mode
// only), 8 trap value (mtval/stval), 9 spare.

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
    beqz t1, srs2_m_park     // zero: unplanned trap, park
    csrw mepc, t1            // resume at the continuation
    li t1, (3 << 11)         // MPP = M-mode: mret takes the privilege
    csrr t2, mstatus         //   from MPP, and the trap arrived from
    or t2, t2, t1            //   S-mode (MPP = 01), so without this the
    csrw mstatus, t2         //   mret would drop back into S-mode
    ld t2, 6*8(t0)           // restore the interrupted t2
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, mscratch, t0   // restore t0, park &m_regs back in mscratch
    mret
srs2_m_park:
    wfi
    j srs2_m_park

    .global s_trap_entry
    .align 2
s_trap_entry:
    csrrw t0, sscratch, t0   // t0 = &s_regs, interrupted t0 parked in sscratch
    ld t1, 0*8(t0)           // bump the trap counter in slot 0
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, scause
    sd t1, 2*8(t0)           // slot 2: scause of this trap
    csrr t1, sepc
    sd t1, 3*8(t0)           // slot 3: sepc of this trap
    csrr t1, stval
    sd t1, 8*8(t0)           // slot 8: stval of this trap
    // No resume: an S-mode trap here is a failure. Park the hart.
srs2_s_park:
    wfi
    j srs2_s_park
