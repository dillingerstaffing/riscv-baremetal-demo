// tor_bot_trap.S: M-mode trap entry for the PMP TOR inclusive-bottom-bound test.
//
// mscratch points at the 8-word tor_bot_save array in tor_bot_main.c:
// [0] total traps seen, [1] saved t1, [2] mcause, [3] mepc,
// [4] mtval, [5] resume pc, [6] seen flag, [7] (unused).
//
// The probes run in M-mode against a LOCKED deny entry, so no
// MPRV trickery is needed: a locked entry applies to M-mode
// accesses too, which is exactly what is under test. The handler
// therefore does no privilege-mode juggling at all: it stashes t0
// via mscratch, records mcause/mepc/mtval, bumps the total-trap
// counter, sets mepc to the resume address the C code stored,
// flags the trap as seen, restores t0/t1, and returns with mret.
// No interrupts are enabled in this module, so every trap here is
// a synchronous access fault from a probe.

    .section .text
    .global tor_bot_trap_entry
    .align 2
tor_bot_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &tor_bot_save, old t0 parked in mscratch
    sd t1, 1*8(t0)           // save[1]: t1
    csrr t1, mcause
    sd t1, 2*8(t0)           // save[2]: mcause
    csrr t1, mepc
    sd t1, 3*8(t0)           // save[3]: mepc
    csrr t1, mtval
    sd t1, 4*8(t0)           // save[4]: mtval
    ld t1, 0*8(t0)           // save[0]: total traps
    addi t1, t1, 1
    sd t1, 0*8(t0)
    ld t1, 5*8(t0)           // save[5]: resume pc, stored by C before the access
    csrw mepc, t1
    li t1, 1
    sd t1, 6*8(t0)           // save[6]: seen flag
    ld t1, 1*8(t0)           // restore t1
    // Restore the interrupted t0 and put &tor_bot_save back into mscratch in
    // one swap: t0 still holds &tor_bot_save here, mscratch holds the old t0.
    // (A plain csrr would leave mscratch clobbered, breaking the next trap.)
    csrrw t0, mscratch, t0
    mret
