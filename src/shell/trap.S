// trap.S: M-mode trap entry for the UART shell module.
//
// On any trap, saves x1-x31 plus mepc/mstatus/mcause into trap_bank
// (mscratch holds the bank address). For an M-mode ecall (mcause 11),
// the handler advances mepc past the ecall and resumes with mret, so a
// C caller can snapshot the exact register state at the ecall site.
// No interrupts are enabled in this module, so ecall is the only
// expected trap; anything else parks the hart.

    .section .text
    .global trap_entry
    .align 2
trap_entry:
    csrrw t0, mscratch, t0   // t0 = &trap_bank, mscratch = interrupted t0
    sd x1,  0*8(t0)
    sd x2,  1*8(t0)
    sd x3,  2*8(t0)
    sd x4,  3*8(t0)
    // x5 (t0) is recovered from mscratch below
    sd x6,  5*8(t0)
    sd x7,  6*8(t0)
    sd x8,  7*8(t0)
    sd x9,  8*8(t0)
    sd x10, 9*8(t0)
    sd x11, 10*8(t0)
    sd x12, 11*8(t0)
    sd x13, 12*8(t0)
    sd x14, 13*8(t0)
    sd x15, 14*8(t0)
    sd x16, 15*8(t0)
    sd x17, 16*8(t0)
    sd x18, 17*8(t0)
    sd x19, 18*8(t0)
    sd x20, 19*8(t0)
    sd x21, 20*8(t0)
    sd x22, 21*8(t0)
    sd x23, 22*8(t0)
    sd x24, 23*8(t0)
    sd x25, 24*8(t0)
    sd x26, 25*8(t0)
    sd x27, 26*8(t0)
    sd x28, 27*8(t0)
    sd x29, 28*8(t0)
    sd x30, 29*8(t0)
    sd x31, 30*8(t0)
    csrr t1, mscratch
    sd t1, 4*8(t0)           // x5 (t0) slot
    csrr t1, mepc
    sd t1, 31*8(t0)
    csrr t1, mstatus
    sd t1, 32*8(t0)
    csrr t1, mcause
    sd t1, 33*8(t0)
    li t2, 11               // environment call from M-mode
    bne t1, t2, trap_halt
    csrr t1, mepc
    addi t1, t1, 4          // ecall is always 4 bytes; resume after it
    csrw mepc, t1
    ld t0, 4*8(t0)          // restore interrupted t0
    mret

trap_halt:
    wfi
    j trap_halt

    .section .bss
    .align 3
    .global trap_bank
trap_bank:
    .space 34*8            // x1-x31 (31), mepc, mstatus, mcause
