// scb_trap.S: trap entries for the scause INTERRUPT-bit module.
//
// s_trap_entry (S-mode, direct-mode stvec): sscratch points at the
// 12-word s_regs array. Bumps the trap counter, records scause/sepc/
// stval of every trap, then dispatches:
//
//   scause == 13 (load page fault): record the scause in slot 7 and
//       stval in slot 10 (sticky, first fault wins), advance sepc by
//       4 past the faulting 4-byte ld, and sret. Exceptions trap
//       regardless of sstatus.SIE, so the fault phase runs with SIE
//       clear and cannot be confused with the timer phase.
//   scause == 0x8000000000000005 (supervisor timer interrupt):
//       record the scause in slot 8, disarm the source by writing
//       all-ones to stimecmp (CSR 0x14d), raise the done flag whose
//       address the C code placed in slot 5, and sret.
//   anything else: bump the unexpected counter in slot 9 and sret
//       (the C phase treats any nonzero value as a failure).
//
// m_trap_entry (M-mode, direct-mode mtvec): bumps m_regs[0] and
// records mcause in m_regs[2], then parks the hart. No trap should
// reach M-mode during the run; a nonzero m_regs[0] fails the run.
//
// Slot map (s_regs): 0 trap count, 1 parked t1, 2 last scause,
// 3 last sepc, 4 last stval, 5 &s_timer_done, 6 parked t2,
// 7 fault scause (sticky), 8 timer scause (sticky),
// 9 unexpected count, 10 fault stval (sticky).

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
    csrrw t0, sscratch, t0   // t0 = &s_regs, interrupted t0 in sscratch
    sd t1, 1*8(t0)           // park interrupted t1 in slot 1
    sd t2, 6*8(t0)           // park interrupted t2 in slot 6
    ld t1, 0*8(t0)           // bump the trap counter in slot 0
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, scause
    sd t1, 2*8(t0)           // slot 2: scause of this trap
    csrr t1, sepc
    sd t1, 3*8(t0)           // slot 3: sepc of this trap
    csrr t1, stval
    sd t1, 4*8(t0)           // slot 4: stval of this trap
    ld t1, 2*8(t0)           // t1 = scause for the dispatch
    li t2, 13
    beq t1, t2, s_fault      // load page fault
    li t2, 0x8000000000000005
    beq t1, t2, s_timer      // supervisor timer interrupt
    ld t1, 9*8(t0)           // unexpected scause: bump slot 9
    addi t1, t1, 1
    sd t1, 9*8(t0)
    j s_done
s_fault:
    ld t1, 7*8(t0)           // sticky: keep the first fault's values
    bne t1, zero, s_fault_skip
    ld t1, 2*8(t0)
    sd t1, 7*8(t0)           // slot 7: fault scause
    ld t1, 4*8(t0)
    sd t1, 10*8(t0)          // slot 10: fault stval
s_fault_skip:
    csrr t1, sepc
    addi t1, t1, 4           // resume past the 4-byte faulting ld
    csrw sepc, t1
    j s_done
s_timer:
    ld t1, 2*8(t0)
    sd t1, 8*8(t0)           // slot 8: timer interrupt scause
    li t1, -1
    csrw 0x14d, t1           // stimecmp = all-ones: disarm the source
    ld t1, 5*8(t0)           // t1 = &s_timer_done
    li t2, 1
    sd t2, 0(t1)             // raise the done flag
s_done:
    ld t2, 6*8(t0)           // restore the interrupted t2
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, sscratch, t0   // restore t0, park &s_regs in sscratch
    sret
