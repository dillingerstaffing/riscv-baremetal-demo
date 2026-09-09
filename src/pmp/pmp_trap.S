// pmp_trap.S: M-mode trap entry for the PMP denial test.
//
// mscratch points at the 7-word pmp_save array in pmp_main.c. On entry
// the handler swaps t0 with mscratch, records mcause/mepc/mtval into the
// array, sets mepc to the resume address the C code stored there, flags
// the trap as seen, restores t0/t1, and returns with mret. No interrupts
// are enabled in this module, so every trap here is a synchronous fault
// from the test's own load/store.

    .section .text
    .global pmp_trap_entry
    .align 2
pmp_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &pmp_save, old t0 parked in mscratch
    sd t1, 1*8(t0)           // save[1]: t1
    csrr t1, mcause
    sd t1, 2*8(t0)           // save[2]: mcause
    csrr t1, mepc
    sd t1, 3*8(t0)           // save[3]: mepc
    csrr t1, mtval
    sd t1, 4*8(t0)           // save[4]: mtval
    ld t1, 5*8(t0)           // save[5]: resume pc, stored by C before the access
    csrw mepc, t1
    li t1, 1
    sd t1, 6*8(t0)           // save[6]: seen flag
    ld t1, 1*8(t0)           // restore t1
    // Restore the interrupted t0 and put &pmp_save back into mscratch in
    // one swap: t0 still holds &pmp_save here, mscratch holds the old t0.
    // (A plain csrr would leave mscratch clobbered, breaking the next trap.)
    csrrw t0, mscratch, t0
    mret
