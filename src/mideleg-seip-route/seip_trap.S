// seip_trap.S: trap entries for the mideleg SEIP-routing module.
//
// m_trap_entry (M-mode): mscratch points at the 8-word m_regs array.
// Bumps the trap counter and records mcause/mepc. No M-mode trap is
// expected in this module (mie and mstatus.MIE are clear throughout
// the S-mode phases); the handler only records so an unexpected trap
// fails the checks honestly instead of hanging. Restores t0/t1/t2,
// mret.
//
// s_trap_entry (S-mode): sscratch points at the 8-word s_regs array.
// Bumps the trap counter, records scause/sepc and the sip value at
// handler entry, clears sstatus.SIE and sstatus.SPIE, raises the
// done flag whose address the C code placed in s_regs[5], restores
// t0/t1/t2, sret.
//
// The SPIE clear is load-bearing: SEIP (mip bit 9) is read-only for
// S-mode (measured by the shipped src/sip-seip-write module), so the
// handler cannot clear the pending bit the way the SSIP sibling
// clears sip.SSIP. A level-triggered SEI would re-fire on sret if
// the gate stayed open; sret restores SIE from SPIE, so clearing
// SIE alone would be undone on return. Clearing both keeps the
// still-pending SEI masked, and the S-mode main code verifies the
// counts never move afterwards.
//
// Slot map (both arrays): 0 trap count, 1 parked t1, 2 cause, 3 pc,
// 4 (S-mode) sip at entry, 5 (S-mode) &s_done, 6 parked t2.

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
    ld t2, 6*8(t0)           // restore the interrupted t2
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, mscratch, t0   // restore t0, park &m_regs back in mscratch
    mret

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
