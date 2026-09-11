// umede_trap.S: trap entries for the medeleg bit-8 U-mode ecall route
// module.
//
// m_trap_entry (M-mode): mscratch points at the 8-word m_regs array.
// Bumps the trap counter and records mcause/mepc and the mstatus
// value at entry. The only M-mode trap this run can take is the
// restore ecall issued from the S-mode reporter (medeleg bit 9 is
// clear, so it is not delegated): the handler then writes
// boot_medeleg back into medeleg, records the readback, points mepc
// at m_finalize with MPP = M-mode, and mret's into it. If an M-mode
// trap fires before the reporter arms the restore (restore_armed ==
// 0), the U-mode ecall was not delegated: the handler records it,
// raises the premature flag in slot 7, and resumes at m_finalize,
// which prints the evidence and fails the run instead of parking
// silently.
//
// s_trap_entry (S-mode): sscratch points at the 8-word s_regs array.
// Bumps the trap counter, records scause/sepc and the sstatus value
// at entry, then redirects: sepc = s_report (the S-mode reporter),
// SPP forced to 1, sret. The U-mode payload's ecall is skipped over
// by the redirect, so the payload code after the ecall never runs.
//
// Slot map (both arrays): 0 trap count, 1 parked t1, 2 cause, 3 pc,
// 4 status-at-entry, 5 (S-mode) &s_done / (M-mode) spare, 6 parked
// t2, 7 (M-mode) premature flag / (S-mode) spare.

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
    // Load restore_armed (a C global, address passed via slot 5
    // would complicate the slot map; use la against the symbol).
    la t1, restore_armed
    ld t1, 0(t1)
    beqz t1, umede_m_premature
    // Restore path: write boot_medeleg back, read it back into
    // restore_rb, resume at m_finalize in M-mode.
    la t1, boot_medeleg
    ld t1, 0(t1)
    csrw medeleg, t1
    csrr t1, medeleg
    la t2, restore_rb
    sd t1, 0(t2)
    la t1, m_finalize
    csrw mepc, t1
    li t1, (3 << 11)         // MPP = M-mode for the mret
    csrr t2, mstatus
    or t2, t2, t1
    csrw mstatus, t2
    j umede_m_ret
umede_m_premature:
    // Premature M-mode trap: the U-mode ecall was not delegated.
    // Raise the flag in slot 7 and resume at m_finalize, which
    // reports the evidence and fails.
    li t1, 1
    sd t1, 7*8(t0)
    la t1, m_finalize
    csrw mepc, t1
    li t1, (3 << 11)
    csrr t2, mstatus
    or t2, t2, t1
    csrw mstatus, t2
umede_m_ret:
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
    sd t1, 3*8(t0)           // slot 3: sepc of this trap (the U-mode ecall)
    csrr t1, sstatus
    sd t1, 4*8(t0)           // slot 4: sstatus at handler entry
    ld t1, 5*8(t0)           // t1 = &s_done (placed there by the C setup)
    li t2, 1
    sd t2, 0(t1)             // raise the done flag
    la t1, s_report          // redirect into the S-mode reporter
    csrw sepc, t1
    csrr t1, sstatus
    ori t1, t1, (1 << 8)     // SPP = 1: the sret below lands in S-mode
    csrw sstatus, t1
    ld t2, 6*8(t0)           // restore the interrupted t2
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, sscratch, t0   // restore t0, park &s_regs back in sscratch
    sret
