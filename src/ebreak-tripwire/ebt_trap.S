// ebt_trap.S: M-mode trap entry for the ebreak-tripwire module.
//
// mscratch points at the 20-word ebt_regs array in ebt_main.c. On entry
// the handler swaps t0 with mscratch, parks t1/t2 in the spare slots,
// selects the record for the current trap (record = trap count, 3 words
// each: mcause, entry mepc, resume address), records mcause and the
// mepc the hardware saved at trap entry, loads mepc from the resume
// address the test stored in the record before the tripwire, bumps the
// trap counter, restores t1/t2 and the symmetric t0 swap, and executes
// mret. The handler touches only t0/t1/t2, all saved and restored, so
// the interrupted context survives the trap.

    .section .text
    .option push
    .option norvc
    .global ebt_trap_entry
    .align 2
ebt_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &ebt_regs, interrupted t0 parked in mscratch
    sd t1, 18*8(t0)          // park t1
    sd t2, 19*8(t0)          // park t2
    ld t1, 16*8(t0)          // trap count: 0 = ebreak, 1 = c.ebreak
    slli t2, t1, 1           // count * 2 ...
    add t1, t1, t2           // ... = count * 3 words ...
    slli t1, t1, 3           // ... = count * 24 bytes
    add t1, t1, t0           // t1 = &record[count]
    csrr t2, mcause
    sd t2, 0*8(t1)           // record: mcause
    csrr t2, mepc
    sd t2, 1*8(t1)           // record: mepc at trap entry
    ld t2, 2*8(t1)           // resume address, stored by the test before the tripwire
    csrw mepc, t2
    ld t2, 19*8(t0)          // restore t2
    ld t1, 16*8(t0)          // trap count
    addi t1, t1, 1
    sd t1, 16*8(t0)          // count++
    ld t1, 18*8(t0)          // restore t1
    csrrw t0, mscratch, t0   // symmetric swap: interrupted t0 back, &ebt_regs into mscratch
    mret
    .option pop
