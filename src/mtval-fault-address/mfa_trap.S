// mfa_trap.S: M-mode trap entry for the mtval-fault-address module.
//
// mscratch points at the 12-word mfa_regs array in mfa_main.c. On entry
// the handler swaps t0 with mscratch, parks t1/t2 in the spare slots,
// reads the trap counter, and records mcause, mtval, and the mepc the
// hardware saved at trap entry into that trap's 4-word record (record
// 0 for the first trap, the load fault; record 1 for the second, the
// store fault). It then loads mepc from the resume address the test
// stored in the record before the fault, bumps the trap counter,
// restores t2, t1, and t0 (the symmetric swap puts &mfa_regs back into
// mscratch), and executes mret. The handler touches only t0/t1/t2, all
// saved and restored, so a0/a1 survive the trap untouched.

    .section .text
    .option push
    .option norvc
    .global mfa_trap_entry
    .align 2
mfa_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &mfa_regs, interrupted t0 parked in mscratch
    sd t1, 9*8(t0)           // park t1
    sd t2, 10*8(t0)          // park t2
    ld t1, 8*8(t0)           // trap count: 0 = load fault, 1 = store fault
    slli t1, t1, 5           // count * 32 = byte offset of this trap's record
    add t1, t1, t0           // t1 = &record[count]
    csrr t2, mcause
    sd t2, 0*8(t1)           // record: mcause
    csrr t2, mtval
    sd t2, 1*8(t1)           // record: mtval
    csrr t2, mepc
    sd t2, 2*8(t1)           // record: mepc at trap entry
    ld t2, 3*8(t1)           // resume address, stored by the test before the fault
    csrw mepc, t2
    ld t2, 10*8(t0)          // restore t2
    ld t1, 8*8(t0)           // trap count
    addi t1, t1, 1
    sd t1, 8*8(t0)           // count++
    ld t1, 9*8(t0)           // restore t1
    csrrw t0, mscratch, t0   // symmetric swap: interrupted t0 back, &mfa_regs into mscratch
    mret
    .option pop
