// stvm_trap.S: trap entries for the stvec-vectored-mode module.
//
// Two hardware entries live in stvm_table, one 4-byte jal stub per
// slot. In vectored mode (stvec MODE=1) the hart jumps to BASE +
// 4*cause, so synchronous exceptions always enter at the slot-0 stub
// (BASE) and the supervisor software interrupt enters at the slot-1
// stub (BASE+4). Each stub is `jal t0, stvm_s_entry`: the jal stores
// the stub's own address + 4 in t0, so the common recorder recovers
// the exact hardware entry pc as t0 - 4 and stores it (s_regs[7]).
// That makes the landing address itself a measured quantity, not an
// assumption.
//
// stvm_s_entry: sscratch points at the 8-word s_regs array. Bumps the
// trap counter, records scause/sepc, the sip value at entry, and the
// vector entry pc. Interrupt path (scause bit 63 set): clears
// sip.SSIP (the level-triggered source would re-fire on sret).
// Exception path: only code 2 (illegal instruction) is expected; the
// 4-byte illegal word is skipped by advancing sepc, then a done flag
// (address placed by the C code in s_regs[5]) is raised. Any other
// exception code stores 0xbad in slot 6 and raises done so the C
// checks fail on values instead of hanging. Restores t0/t1/t2, sret.
//
// m_trap_entry (M-mode): mscratch points at m_regs. Bumps the trap
// counter and records mcause/mepc. No M-mode trap is expected (mie
// and mstatus.MIE are clear for the whole S-mode phase); the handler
// only records so an unexpected trap fails the checks honestly
// instead of hanging. Restores t0/t1/t2, mret.
//
// Slot map (s_regs): 0 trap count, 1 parked t1, 2 scause, 3 sepc,
// 4 sip at entry, 5 &s_done, 6 unexpected-exception marker,
// 7 vector entry pc (BASE + 4*cause).

    .section .text
    .global stvm_table
    .align 2
stvm_table:
    jal t0, stvm_s_entry   // slot 0: BASE, every synchronous exception
    jal t0, stvm_s_entry   // slot 1: BASE+4, supervisor software interrupt

    .global stvm_s_entry
    .align 2
stvm_s_entry:
    csrrw t2, sscratch, t2   // t2 = &s_regs, interrupted t2 parked
    sd t1, 1*8(t2)           // park interrupted t1
    addi t0, t0, -4          // t0 = hardware entry pc (BASE + 4*cause)
    sd t0, 7*8(t2)           // slot 7: the landing address, measured
    ld t0, 0*8(t2)           // bump the trap counter
    addi t0, t0, 1
    sd t0, 0*8(t2)
    csrr t0, scause
    sd t0, 2*8(t2)           // slot 2: scause
    csrr t0, sepc
    sd t0, 3*8(t2)           // slot 3: sepc
    csrr t0, sip
    sd t0, 4*8(t2)           // slot 4: sip at handler entry
    csrr t0, scause
    bltz t0, 1f              // bit 63 set: interrupt path
    // Exception path: only the illegal-instruction code 2 is
    // expected (medeleg delegates exactly bit 2).
    slli t0, t0, 1
    srli t0, t0, 1
    li t1, 2
    bne t0, t1, 3f           // anything else: unexpected
    csrr t0, sepc
    addi t0, t0, 4           // skip the 4-byte illegal word
    csrw sepc, t0
    j 2f
1:  // Interrupt path: clear the level-triggered SSIP source so the
    // interrupt fires exactly once.
    li t0, 2
    csrc sip, t0
2:  ld t0, 5*8(t2)           // t0 = &s_done
    li t1, 1
    sd t1, 0(t0)             // raise the done flag
    ld t1, 1*8(t2)           // restore the interrupted t1
    csrrw t2, sscratch, t2   // restore t2, park &s_regs back
    sret
3:  // Unexpected exception code: marker in slot 6, raise done, sret
    // so the C-side checks fail on values rather than hanging.
    li t0, 0xbad
    sd t0, 6*8(t2)
    j 2b

    .global m_trap_entry
    .align 2
m_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &m_regs
    sd t1, 1*8(t0)           // park interrupted t1
    sd t2, 6*8(t0)           // park interrupted t2
    ld t1, 0*8(t0)           // bump the trap counter
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, mcause
    sd t1, 2*8(t0)           // slot 2: mcause
    csrr t1, mepc
    sd t1, 3*8(t0)           // slot 3: mepc
    ld t2, 6*8(t0)           // restore the interrupted t2
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, mscratch, t0   // restore t0, park &m_regs back
    mret
