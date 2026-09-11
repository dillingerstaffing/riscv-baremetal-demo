// dvm_trap.S: M-mode direct-mode trap entry for the mtvec direct
// vectoring module.
//
// mscratch points at the dvm_regs array in dvm_main.c. On entry the
// handler swaps t0 through mscratch (the interrupted t0 is parked
// in mscratch), parks the interrupted t1 and t2 in save slots, then
// dispatches on mcause. Code 2 (illegal instruction) and code 11
// (M-mode ecall) each record the exact link-time address of this
// entry (the landing address the hardware chose), mcause, and mepc
// into their own slots, and bump their own counter. Any other cause
// bumps the unexpected counter and records its mcause; main()'s
// checks fail the run on that path. Every path resumes at mepc+4
// (both faulting sites are 4-byte instructions), restores the
// interrupted t0/t1/t2, and returns with mret.
//
// dvm_regs layout:
//   [0..2] illegal trap: landing, mcause, mepc
//   [3]    illegal count
//   [4]    ecall count
//   [5]    unexpected count, [6] unexpected mcause
//   [7..9] ecall trap: landing, mcause, mepc
//   [10]   saved interrupted t1, [11] saved interrupted t2

    .section .text
    .align 2
    .global dvm_trap_entry
dvm_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &dvm_regs, interrupted t0 parked
    sd t1, 10*8(t0)          // park interrupted t1
    sd t2, 11*8(t0)          // park interrupted t2
    csrr t1, mcause
    li t2, 2
    beq t1, t2, .Ldvm_ill
    li t2, 11
    beq t1, t2, .Ldvm_ecall
    // Unexpected cause: count it and record its mcause. main()
    // fails the run if this path ever ran.
    ld t2, 5*8(t0)
    addi t2, t2, 1
    sd t2, 5*8(t0)
    sd t1, 6*8(t0)
    j .Ldvm_resume
.Ldvm_ill:
    ld t2, 3*8(t0)
    addi t2, t2, 1
    sd t2, 3*8(t0)           // illegal-instruction count
    la t2, dvm_trap_entry    // exact entry address, assembler-resolved
    sd t2, 0*8(t0)           // landing observed by this trap
    sd t1, 1*8(t0)           // mcause (still in t1)
    csrr t2, mepc
    sd t2, 2*8(t0)           // mepc
    j .Ldvm_resume
.Ldvm_ecall:
    ld t2, 4*8(t0)
    addi t2, t2, 1
    sd t2, 4*8(t0)           // ecall count
    la t2, dvm_trap_entry    // exact entry address, assembler-resolved
    sd t2, 7*8(t0)           // landing observed by this trap
    sd t1, 8*8(t0)           // mcause (still in t1)
    csrr t2, mepc
    sd t2, 9*8(t0)           // mepc
.Ldvm_resume:
    csrr t1, mepc
    addi t1, t1, 4           // both faulting sites are 4-byte insns
    csrw mepc, t1
    ld t1, 10*8(t0)          // restore interrupted t1
    ld t2, 11*8(t0)          // restore interrupted t2
    csrrw t0, mscratch, t0   // restore interrupted t0, park &dvm_regs
    mret
