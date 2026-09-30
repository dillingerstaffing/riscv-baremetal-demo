// mhr_trap.S: M-mode trap entry for the mhartid read-only check.
//
// mscratch points at the 7-word mhr_regs array in mhr_main.c. On
// entry the handler swaps t0 with mscratch, bumps the trap counter
// in word 0, and records mcause, the mepc value the hardware saved
// at trap entry, and mtval into this trap's 3-word slot (slot 0 is
// words 1..3, slot 1 is words 4..6). If more than two traps fire,
// further records are dropped (the count keeps going, which the
// main program fails on) rather than overflowing the array. It then
// advances mepc past the faulting instruction and resumes with
// mret. The advance is length-aware: it reads the 16-bit parcel at
// mepc and skips 2 bytes for a 16-bit instruction, 4 bytes
// otherwise. The faulting instructions in this module are csrw
// (always 32-bit), so the resume lands on the instruction
// immediately after the csrw.
//
// Only t0 (scratch-swapped) and t1/t2/t3/t4 (clobbered, documented)
// are touched; all other registers are preserved across the trap.

    .section .text
    .global mhr_trap_entry
    .align 2
mhr_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &mhr_regs, interrupted t0 parked in mscratch
    ld t1, 0(t0)             // t1 = trap count so far
    addi t2, t1, 1
    sd t2, 0(t0)             // count++
    li t3, 2
    bgeu t1, t3, mhr_advance // 2 records already kept: skip recording
    slli t2, t1, 4           // count * 16
    slli t3, t1, 3           // count * 8
    add t2, t2, t3           // count * 24
    addi t2, t2, 8           // + 8: this trap's slot offset in bytes
    add t1, t2, t0           // t1 = &slot for this trap
    csrr t2, mcause
    sd t2, 0(t1)             // slot[0] = mcause
    csrr t2, mepc
    sd t2, 8(t1)             // slot[1] = mepc at trap entry
    csrr t2, mtval
    sd t2, 16(t1)            // slot[2] = mtval
mhr_advance:
    // Advance mepc past the faulting instruction.
    csrr t2, mepc
    lhu t3, 0(t2)            // 16-bit parcel at mepc
    andi t3, t3, 3
    li t4, 3
    bne t3, t4, mhr_skip2
    addi t2, t2, 4
    j mhr_resume
mhr_skip2:
    addi t2, t2, 2
mhr_resume:
    csrw mepc, t2
    csrrw t0, mscratch, t0   // symmetric swap: interrupted t0 back
    mret
