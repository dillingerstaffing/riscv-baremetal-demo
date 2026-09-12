// sppu_trap.S: trap entries for the sstatus-spp-u-trap module.
//
// s_trap_entry (S-mode, direct-mode stvec): sscratch points at the
// 16-word spu_regs array. Bumps the trap counter, records scause/sepc/
// stval and sstatus.SPP (bit 8) of the trap, then dispatches:
//
//   scause == 8 (environment call from U-mode) and trap count == 1:
//       copy SPP, scause and sepc into the sticky slots 6/7/8,
//       advance sepc by 4 past the ecall, and sret back to U-mode
//       (sstatus.SPP is 0, so sret returns to U-mode).
//
//   Anything else (wrong scause, or a second trap): park the hart.
//   The bench harness runs QEMU under `timeout`, so the park is
//   observable as the timeout exit status (124).
//
// m_trap_entry (M-mode, direct-mode mtvec): bumps m_regs[0] and
// records mcause in m_regs[2], then parks the hart. No trap should
// reach M-mode during the run (the U-mode ecall is delegated via
// medeleg bit 8); a parked M-mode trap is a hard failure.
//
// Slot map (spu_regs): 0 trap count, 1 parked t1, 2 last scause,
// 3 last sepc, 4 last stval, 5 parked t2, 6 trap SPP, 7 trap scause,
// 8 trap sepc, 13 SPP of this trap (temp).
//
// Only t0/t1/t2 are used inside the S-mode handler, all three parked
// and restored, so the interrupted U-mode payload sees no register
// change.

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
    csrrw t0, sscratch, t0   // t0 = &spu_regs, interrupted t0 in sscratch
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
    li t2, 8                 // environment call from U-mode
    bne t1, t2, s_unexpected
    ld t1, 0*8(t0)           // t1 = trap count
    addi t1, t1, -1
    bnez t1, s_unexpected    // anything past the first trap: not scripted
    ld t2, 13*8(t0)          // t2 = SPP of this trap
    sd t2, 6*8(t0)           // sticky: trap SPP
    ld t1, 2*8(t0)
    sd t1, 7*8(t0)           // sticky: trap scause
    ld t1, 3*8(t0)
    sd t1, 8*8(t0)           // sticky: trap sepc
    csrr t1, sepc            // ecall is always 4 bytes; resume past it
    addi t1, t1, 4
    csrw sepc, t1
    ld t1, 1*8(t0)           // restore the interrupted t1
    ld t2, 5*8(t0)           // restore the interrupted t2
    // One swap restores the interrupted t0 and parks &spu_regs back
    // in sscratch for the next trap.
    csrrw t0, sscratch, t0
    sret                     // sstatus.SPP is 0: returns to U-mode
s_unexpected:
    ld t1, 1*8(t0)
    ld t2, 5*8(t0)
    csrrw t0, sscratch, t0
s_park:                      // not the scripted trap: park the hart
    wfi
    j s_park
