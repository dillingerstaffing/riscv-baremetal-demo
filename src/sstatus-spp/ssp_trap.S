// ssp_trap.S: trap entries for the sstatus-spp module.
//
// s_trap_entry (S-mode, direct-mode stvec): sscratch points at the
// 16-word ssp_regs array. Bumps the trap counter, records scause/sepc/
// stval and sstatus.SPP (bit 8) of every trap, then dispatches:
//
//   scause == 9 (environment call from S-mode): copy SPP, scause and
//       sepc into the sticky per-trap slots (trap 1 -> slots 6/7/8,
//       trap 2 -> slots 9/10/11), advance sepc by 4 past the ecall,
//       and sret. A third trap, or any scause other than 9, parks the
//       hart: the bench harness runs QEMU under `timeout`, so the
//       park is observable as the timeout exit status (124).
//
// m_trap_entry (M-mode, direct-mode mtvec): bumps m_regs[0] and
// records mcause in m_regs[2], then parks the hart. No trap should
// reach M-mode during the run (the S-mode ecall is delegated via
// medeleg bit 9); a parked M-mode trap is a hard failure.
//
// Slot map (ssp_regs): 0 trap count, 1 parked t1, 2 last scause,
// 3 last sepc, 4 last stval, 5 parked t2, 6 trap1 SPP, 7 trap1
// scause, 8 trap1 sepc, 9 trap2 SPP, 10 trap2 scause, 11 trap2 sepc,
// 13 SPP of this trap (temp).
//
// Only t0/t1/t2 are used inside the S-mode handler, all three parked
// and restored, so the interrupted payload sees no register change.

    .section .text
    .global m_trap_entry
    .align 2
m_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &m_regs, park interrupted t0
    sd t1, 1*8(t0)
    ld t1, 0*8(t0)           // bump the M-mode trap counter
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, mcause
    sd t1, 2*8(t0)           // slot 2: mcause of this trap
    ld t1, 1*8(t0)
    csrrw t0, mscratch, t0
m_park:                      // an M-mode trap is a hard failure: park
    wfi
    j m_park

    .global s_trap_entry
    .align 2
s_trap_entry:
    csrrw t0, sscratch, t0   // t0 = &ssp_regs, interrupted t0 in sscratch
    sd t1, 1*8(t0)           // park interrupted t1 in slot 1
    sd t2, 5*8(t0)           // park interrupted t2 in slot 5
    ld t1, 0*8(t0)           // bump the trap counter in slot 0
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, scause
    sd t1, 2*8(t0)           // slot 2: scause of this trap
    csrr t1, sepc
    sd t1, 3*8(t0)           // slot 3: sepc of this trap
    csrr t1, stval
    sd t1, 4*8(t0)           // slot 4: stval of this trap
    csrr t1, sstatus         // sstatus.SPP is bit 8
    srli t1, t1, 8
    andi t1, t1, 1
    sd t1, 13*8(t0)          // slot 13: SPP of this trap
    ld t1, 2*8(t0)           // t1 = scause for the dispatch
    li t2, 9                 // environment call from S-mode
    bne t1, t2, s_unexpected
    ld t1, 0*8(t0)           // t1 = trap count
    ld t2, 13*8(t0)          // t2 = SPP of this trap
    addi t1, t1, -1
    beqz t1, s_first         // count == 1
    addi t1, t1, -1
    beqz t1, s_second        // count == 2
    j s_unexpected           // third trap: not part of the script
s_first:
    sd t2, 6*8(t0)           // sticky: trap1 SPP
    ld t1, 2*8(t0)
    sd t1, 7*8(t0)           // sticky: trap1 scause
    ld t1, 3*8(t0)
    sd t1, 8*8(t0)           // sticky: trap1 sepc
    j s_resume
s_second:
    sd t2, 9*8(t0)           // sticky: trap2 SPP
    ld t1, 2*8(t0)
    sd t1, 10*8(t0)          // sticky: trap2 scause
    ld t1, 3*8(t0)
    sd t1, 11*8(t0)          // sticky: trap2 sepc
    j s_resume
s_resume:
    csrr t1, sepc            // ecall is always 4 bytes; resume past it
    addi t1, t1, 4
    csrw sepc, t1
    ld t1, 1*8(t0)           // restore the interrupted t1
    ld t2, 5*8(t0)           // restore the interrupted t2
    // One swap restores the interrupted t0 and parks &ssp_regs back
    // in sscratch for the next trap.
    csrrw t0, sscratch, t0
    sret
s_unexpected:
    ld t1, 1*8(t0)
    ld t2, 5*8(t0)
    csrrw t0, sscratch, t0
s_park:                      // not the scripted trap: park the hart
    wfi
    j s_park
