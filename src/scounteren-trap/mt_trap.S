// mt_trap.S: M-mode trap entry for the scounteren-trap module.
//
// The only M-mode trap a correct run takes is the S-mode ecall
// (mcause 9), issued after the gating phase to ask M-mode to set
// the mcounteren CY bit. medeleg delegates only the
// illegal-instruction trap (bit 2) to S-mode, so S-mode ecalls
// still arrive here. The handler sets the CY bit, records the
// resulting mcounteren value and the ecall count in C-visible
// globals, advances mepc past the 4-byte ecall (ecall has no
// compressed encoding), and mret's back to S-mode. Any other
// mcause parks the hart, which the bench harness observes as a
// timeout.
//
// The handler clobbers t0/t1/t2 of the interrupted context; the
// S-mode ecall site declares those as clobbers, so the compiler
// keeps nothing live in them across the ecall.

    .section .text
    .option push
    .option norvc
    .global mt_trap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
mt_trap_entry:
    csrr t0, mcause
    li t1, 9                // environment call from S-mode
    bne t0, t1, mt_park
    csrr t1, mcounteren
    ori t1, t1, 1           // set the CY bit
    csrw mcounteren, t1
    la t2, mt_mc_after
    sd t1, 0(t2)
    la t2, mt_traps
    ld t1, 0(t2)
    addi t1, t1, 1
    sd t1, 0(t2)
    csrr t0, mepc
    addi t0, t0, 4
    csrw mepc, t0
    mret
mt_park:
    wfi
    j mt_park
    .option pop
