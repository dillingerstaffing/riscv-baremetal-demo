// midr_trap.S: trap entries for the mideleg selective-routing module.
//
// m_trap_entry (M-mode): mscratch points at the 8-word m_regs array.
// Bumps the trap counter and records mcause for every trap. Timer
// interrupts (the STI path) additionally record mepc and clear the
// pended STIP bit (mip bit 5) so the interrupt fires exactly once.
// An S-mode ecall (mcause 9) is a readback service: the handler
// reports the current mideleg value in m_regs[5] and resumes past
// the ecall, since S-mode cannot read that CSR itself. Restores
// t0/t1/t2 and returns with mret.
//
// s_trap_entry (S-mode): sscratch points at the 8-word s_regs array.
// Bumps the trap counter, records scause/sepc, claims the interrupt
// through the PLIC supervisor-context (context 1) claim register,
// reads the looped-back UART byte (dropping the UART's IRQ line),
// completes the claim, raises the done flag whose address the C code
// placed in s_regs[5], restores t0/t1/t2, and returns with sret.
//
// Slot map (both arrays): 0 trap count, 1 parked t1, 2 cause, 3 pc,
// 4 (S-mode) claimed id, 5 (M-mode) mideleg readback via ecall service
//     / (S-mode) &s_done, 6 parked t2.

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
    li t2, 9
    beq t1, t2, m_ecall      // S-mode ecall: service the readback request
    csrr t1, mepc
    sd t1, 3*8(t0)           // slot 3: mepc of this trap
    li t1, 32                // clear the pended STIP bit (mip bit 5)
    csrc mip, t1             // so the timer interrupt fires exactly once
    j m_done
m_ecall:
    csrr t1, mideleg
    sd t1, 5*8(t0)           // slot 5: mideleg readback for the S-mode caller
    csrr t1, mepc
    addi t1, t1, 4           // resume past the ecall
    csrw mepc, t1
m_done:
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
    li t1, 0x0c201004        // PLIC claim register, hart 0 S-mode context
    lw t1, 0(t1)
    sd t1, 4*8(t0)           // slot 4: claimed interrupt id
    li t1, 0x10000000        // read the looped-back UART byte; this drops
    lbu t1, 0(t1)            // the UART's IRQ line (level-triggered source)
    li t1, 0x0c201004
    li t2, 10                // complete the UART claim
    sw t2, 0(t1)
    ld t1, 5*8(t0)           // t1 = &s_done (placed there by the C setup)
    li t2, 1
    sd t2, 0(t1)             // raise the done flag
    ld t2, 6*8(t0)           // restore the interrupted t2
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, sscratch, t0   // restore t0, park &s_regs back in sscratch
    sret
