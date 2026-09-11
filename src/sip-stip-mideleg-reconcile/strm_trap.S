    // strm_trap.S: M-mode trap entry for the sip.STIP vs mideleg bit 5
    // reconciliation probe.
    //
    // m_trap_entry: the only vector in the system. mideleg delegates
    // only interrupts, so every trap lands here in M-mode. Entry swaps
    // t0 with mscratch so t0 points at the save area, parks
    // t1/t2/t3, bumps the trap count, records mcause/mepc, and
    // advances mepc past the trapping instruction (2 bytes if
    // compressed, else 4) so a surprise trap cannot loop silently.
    //
    // The deliberate ecall that ends Phase A is expected to arrive
    // with count 1 and mcause 9 (S-mode environment call). The
    // handler recognizes exactly that trap: it sets mideleg bit 5
    // (STI delegation for Phase B), records the new mideleg readback
    // in mideleg_phase_b, and raises the phase_b flag. Any other trap
    // is only recorded and skipped past; the S-mode checks then catch
    // it because the trap count or mcause will not match the
    // deliberate-transition values.
    //
    // No stack is used; everything lives in the mscratch save area or
    // in the two .bss globals. mstatus.MPP is set to S-mode by the
    // trap, so mret resumes in S-mode at the advanced mepc.

    .section .text
    .global m_trap_entry
    .align 2

m_trap_entry:
    csrrw t0, mscratch, t0      // t0 = save area, mscratch = interrupted t0
    sd t1, 8(t0)
    sd t2, 40(t0)
    sd t3, 48(t0)

    ld t1, 16(t0)              // bump the trap count
    addi t1, t1, 1
    sd t1, 16(t0)

    csrr t1, mcause            // record what fired, for the verdict print
    sd t1, 24(t0)
    csrr t1, mepc
    sd t1, 32(t0)

    // Skip the trapping instruction: 2 bytes when the low two opcode
    // bits say compressed, 4 bytes otherwise.
    lhu t2, 0(t1)
    andi t2, t2, 3
    li t3, 3
    li t1, 2
    bne t2, t3, 1f
    li t1, 4
1:
    ld t2, 32(t0)              // mepc
    add t2, t2, t1
    csrw mepc, t2

    // Phase transition: recognize the deliberate end-of-Phase-A
    // ecall by count == 1 and mcause == 9 (S-mode ecall). Only then
    // delegate STI for Phase B.
    ld t1, 16(t0)              // trap count
    li t2, 1
    bne t1, t2, 2f
    ld t1, 24(t0)              // mcause
    li t2, 9
    bne t1, t2, 2f
    csrr t1, mideleg           // set mideleg bit 5 (STI)
    ori t1, t1, 0x20
    csrw mideleg, t1
    csrr t1, mideleg
    la t2, mideleg_phase_b
    sd t1, 0(t2)               // publish the Phase B mideleg readback
    li t2, 1
    la t1, phase_b
    sd t2, 0(t1)               // raise the phase_b flag
2:
    ld t3, 48(t0)
    ld t2, 40(t0)
    ld t1, 8(t0)
    csrrw t0, mscratch, t0      // restore t0, reinstall the save area
    mret
