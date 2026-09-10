// mnr_trap.S: M-mode trap entry for the mret-no-restore module.
//
// mscratch points at the 4-word mnr_regs array in mnr_main.c. On entry
// the handler swaps t0 with mscratch, records mcause/mepc/mtval into
// mnr_regs[0..2], bumps the trap counter in mnr_regs[3], and advances
// mepc past the ecall (ecall is always 4 bytes; this file assembles
// with .option norvc so every trapping instruction here is exactly
// 4 bytes). The mepc advance is the one concession to forward
// progress: without it mret would re-execute the ecall and trap
// forever. Then t0 is restored via the symmetric second swap, and the
// handler writes the POST_A0..POST_A7 sentinel set into a0..a7.
//
// Deliberately no restore after that point: t1 is left holding
// mepc+4, a0..a7 keep the handler's sentinels across mret, and no
// C code runs in the handler at all. The pre-trap caller reads a0..a7
// after mret and compares against the recorded pre-trap values; the
// dumps are the ground truth for the claim.

#include "mnr_vals.h"

    .section .text
    .option push
    .option norvc
    .global mnr_trap_entry
    .align 2
mnr_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &mnr_regs, interrupted t0 parked in mscratch
    csrr t1, mcause
    sd t1, 0*8(t0)
    csrr t1, mepc
    sd t1, 1*8(t0)
    csrr t1, mtval
    sd t1, 2*8(t0)
    ld t1, 3*8(t0)
    addi t1, t1, 1
    sd t1, 3*8(t0)           // trap_count++
    csrr t1, mepc
    addi t1, t1, 4
    csrw mepc, t1            // resume at the instruction after the ecall
    csrrw t0, mscratch, t0   // symmetric swap: interrupted t0 back, &mnr_regs parked
    // No restore from here on. a0..a7 get the handler's sentinel set
    // and mret carries those values back to the pre-trap caller.
    li a0, POST_A0
    li a1, POST_A1
    li a2, POST_A2
    li a3, POST_A3
    li a4, POST_A4
    li a5, POST_A5
    li a6, POST_A6
    li a7, POST_A7
    mret
    .option pop
