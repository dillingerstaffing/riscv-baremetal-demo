// mtip_trap.S: trap entries for the mideleg MTIP-routing module.
//
// m_trap_entry (M-mode): mscratch points at the 8-word m_regs array.
// Counts the trap, records mcause/mepc/mtval and an entry rdcycle
// stamp, disarms the CLINT machine timer by writing mtimecmp (MMIO
// 0x02004000, hart 0) to all-ones so the interrupt fires exactly
// once, raises the m_done flag whose address the C code placed in
// m_regs[5], and returns with mret. Saves the t0/t1/ra/a0 it touches
// so the interrupted S-mode spin loop resumes undisturbed.
//
// s_trap_entry (S-mode): sscratch points at the 8-word s_regs array.
// No S-mode trap is expected in this module: mideleg bit 7 is
// read-only zero on this hart, so the machine timer interrupt cannot
// be delegated and must land in M-mode. Any S-mode trap would be a
// FAIL; the handler counts it, records scause/sepc, disarms the
// timer defensively so it cannot re-fire in a loop, raises the
// s_done flag, and returns with sret.
//
// Slot map (both arrays): 0 trap count, 1 parked t1, 2 cause, 3 pc,
// 4 mtval (M) / sip (S), 5 &done flag, 6 parked ra, 7 parked a0.

    .section .text
    .global m_trap_entry
    .align 2
m_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &m_regs, interrupted t0 parked in mscratch
    sd t1, 1*8(t0)           // park interrupted t1 in slot 1
    ld t1, 0*8(t0)           // bump the trap counter in slot 0
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, mcause
    sd t1, 2*8(t0)           // slot 2: mcause of this trap
    csrr t1, mepc
    sd t1, 3*8(t0)           // slot 3: mepc of this trap
    csrr t1, mtval
    sd t1, 4*8(t0)           // slot 4: mtval
    sd ra, 6*8(t0)           // park ra (the li+call sequence clobbers it)
    sd a0, 7*8(t0)           // park a0
    li t1, 0x02004000        // CLINT mtimecmp, hart 0
    li a0, -1
    sd a0, 0(t1)             // disarm: compare above the running mtime
    ld t1, 5*8(t0)           // t1 = &m_done (placed there by the C setup)
    li a0, 1
    sd a0, 0(t1)             // raise the done flag
    ld ra, 6*8(t0)           // restore the interrupted ra
    ld a0, 7*8(t0)           // restore the interrupted a0
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, mscratch, t0   // restore t0, park &m_regs back in mscratch
    mret

    .global s_trap_entry
    .align 2
s_trap_entry:
    csrrw t0, sscratch, t0   // t0 = &s_regs, interrupted t0 parked in sscratch
    sd t1, 1*8(t0)           // park interrupted t1 in slot 1
    ld t1, 0*8(t0)           // bump the trap counter in slot 0
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, scause
    sd t1, 2*8(t0)           // slot 2: scause of this trap
    csrr t1, sepc
    sd t1, 3*8(t0)           // slot 3: sepc of this trap
    csrr t1, sip
    sd t1, 4*8(t0)           // slot 4: sip at handler entry
    sd ra, 6*8(t0)
    sd a0, 7*8(t0)
    li t1, 0x02004000        // CLINT mtimecmp, hart 0
    li a0, -1
    sd a0, 0(t1)             // defensive disarm so an unexpected trap
                            // cannot re-fire in a loop on sret
    ld t1, 5*8(t0)           // t1 = &s_done (placed there by the C setup)
    li a0, 1
    sd a0, 0(t1)             // raise the done flag
    ld ra, 6*8(t0)
    ld a0, 7*8(t0)
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, sscratch, t0   // restore t0, park &s_regs back in sscratch
    sret
