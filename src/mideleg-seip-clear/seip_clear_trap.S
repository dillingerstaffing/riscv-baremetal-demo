// seip_clear_trap.S: trap entries for the mideleg SEIP clear-and-restore module.
//
// m_trap_entry (M-mode): mscratch points at the 8-word m_regs array.
// Records mcause/mepc on every trap, then:
//   - interrupt (mcause bit 63 set): bumps the interrupt counter in
//     slot 0, clears mstatus.MIE and mstatus.MPIE, mret with mepc
//     unchanged. The MPIE clear is load-bearing: the SEI under test
//     is level-triggered and stays pending through the phase-B
//     trap, and mret restores MIE from MPIE, so without the MPIE
//     clear the interrupt would re-fire on return.
//   - ecall from S-mode (mcause 9): bumps the ecall counter in slot
//     7, sets mepc to the trampoline target the C code placed in
//     m_ecall_target, sets mstatus.MPP to M-mode (the trap had set
//     MPP to S-mode, and mret restores the privilege from MPP),
//     mret. This is how the S-mode phases hand control back to
//     M-mode for the next phase; medeleg is 0, so the ecall traps
//     in M-mode.
//   - anything else: parks in wfi. The C phases poll with a spin
//     budget, so a parked hart shows up as a timeout (exit 124),
//     not a silent wrong result.
//
// s_trap_entry (S-mode): the sibling module's handler unchanged:
// bumps the trap counter, records scause/sepc and the sip value at
// entry, clears sstatus.SIE and sstatus.SPIE (sret restores SIE from
// SPIE, so the SPIE clear is load-bearing against the still-pending
// SEI), raises the done flag whose address the C code placed in
// s_regs[5], sret.
//
// Slot map: m_regs: 0 M-mode interrupt count, 1 parked t1, 2 mcause,
// 3 mepc, 6 parked t2, 7 ecall count. s_regs: 0 S-mode trap count,
// 1 parked t1, 2 scause, 3 sepc, 4 sip at entry, 5 &s_done,
// 6 parked t2.

    .section .text
    .global m_trap_entry
    .align 2
m_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &m_regs, interrupted t0 parked in mscratch
    sd t1, 1*8(t0)           // park interrupted t1 in slot 1
    sd t2, 6*8(t0)           // park interrupted t2 in slot 6
    csrr t1, mcause
    sd t1, 2*8(t0)           // slot 2: mcause of this trap
    csrr t1, mepc
    sd t1, 3*8(t0)           // slot 3: mepc of this trap
    csrr t1, mcause
    bltz t1, 1f              // sign bit set: interrupt, not exception
    li t2, 9
    bne t1, t2, 3f           // not an S-mode ecall: unexpected, park
    // ecall from S-mode: count it separately from interrupts, then
    // jump to the phase trampoline instead of returning to S-mode.
    // The trap hardware set MPP to S-mode; mret restores the
    // privilege from MPP, so set MPP to M-mode first.
    ld t1, 7*8(t0)
    addi t1, t1, 1
    sd t1, 7*8(t0)           // slot 7: ecall count
    la t2, m_ecall_target
    ld t2, 0(t2)
    csrw mepc, t2
    li t1, 0x1800            // MPP mask (bits 12:11)
    csrs mstatus, t1         // MPP = 11, M-mode, for the mret below
    j 2f
1:  // M-mode interrupt: bump the interrupt counter, mask MIE and
    // MPIE against the still-pending level-triggered SEI.
    ld t1, 0*8(t0)
    addi t1, t1, 1
    sd t1, 0*8(t0)           // slot 0: M-mode interrupt count
    li t1, 0x88              // MIE (bit 3) | MPIE (bit 7)
    csrrc zero, mstatus, t1
2:
    ld t2, 6*8(t0)           // restore the interrupted t2
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, mscratch, t0   // restore t0, park &m_regs back in mscratch
    mret
3:  // Unexpected M-mode exception: park. Bounded C-side polls turn
    // this into a timeout (exit 124), never a wrong PASS.
    ld t2, 6*8(t0)
    ld t1, 1*8(t0)
    csrrw t0, mscratch, t0
4:  wfi
    j 4b

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
    sd t1, 3*8(t0)           // slot 3: sepc of this trap
    csrr t1, sip
    sd t1, 4*8(t0)           // slot 4: sip at handler entry
    li t1, 0x22             // SIE (bit 1) | SPIE (bit 5)
    csrrc zero, sstatus, t1 // mask the still-pending SEI: SIE off, and
                            // SPIE off because sret restores SIE from
                            // SPIE, so clearing SIE alone would be
                            // undone on return and the SEI would
                            // re-fire immediately
    ld t1, 5*8(t0)           // t1 = &s_done (placed there by the C setup)
    li t2, 1
    sd t2, 0(t1)             // raise the done flag
    ld t2, 6*8(t0)           // restore the interrupted t2
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, sscratch, t0   // restore t0, park &s_regs back in sscratch
    sret
