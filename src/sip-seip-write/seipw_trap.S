    // seipw_trap.S: M-mode trap entry for the sip.SEIP read-only probe.
    //
    // m_trap_entry: the only vector in the system. mideleg stays zero,
    // so every trap lands here in M-mode. Entry swaps t0 with mscratch
    // so t0 points at the save area, parks t1/t2/t3, bumps the trap
    // count, records mcause/mepc, and advances mepc past the trapping
    // instruction (2 bytes if compressed, else 4) so a surprise trap
    // cannot loop silently; then mret resumes. No trap is expected:
    // the run's checks require the count to stay 0, and any recorded
    // mcause/mepc is printed in the verdict section. No stack is used;
    // everything lives in the mscratch save area.

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

    ld t3, 48(t0)
    ld t2, 40(t0)
    ld t1, 8(t0)
    csrrw t0, mscratch, t0      // restore t0, reinstall the save area
    mret
