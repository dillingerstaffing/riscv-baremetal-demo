// pnm_trap.S: M-mode trap entry for the PMP NAPOT match test.
//
// mscratch points at the 8-word pnm_save array in pnm_main.c:
// [0] total traps seen, [1] saved t1, [2] mcause, [3] mepc,
// [4] mtval, [5] resume pc, [6] seen flag, [7] (written by C, not
// the handler).
//
// The probes run with mstatus.MPRV=1 and MPP=S, so the very first
// thing the handler does after stashing t0 is clear MPRV: with
// MPRV still set, the handler's own data accesses would be
// privilege-checked as the MPP mode. One store to the save area
// happens before that clear, but trap entry already set MPP to the
// previous mode (M), so that one store is privilege-checked as
// M-mode, to which the unlocked entries never apply. pnm_save also
// lives in .bss below 0x80002000, inside the allow entry's 16 KiB
// window, so the store is allowed twice over. The program verifies
// the layout (pnm_save below 0x80002000, live stack 1 KiB clear of
// the region) before it programs the PMP entries, so this stays
// true.
//
// On entry the handler swaps t0 with mscratch, records
// mcause/mepc/mtval, bumps the total-trap counter, sets mepc to
// the resume address the C code stored, flags the trap as seen,
// restores t0/t1, and returns with mret. No interrupts are
// enabled in this module, so every trap here is a synchronous
// access fault from a probe.

    .section .text
    .global pnm_trap_entry
    .align 2
pnm_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &pnm_save, old t0 parked in mscratch
    sd t1, 1*8(t0)           // save[1]: t1 (MPRV still set, but trap
                             // entry set MPP=M, so M-mode-checked)
    li t1, 0x20000           // MPRV bit 17
    csrc mstatus, t1         // MPRV=0 from here on: plain M-mode
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
    // Restore the interrupted t0 and put &pnm_save back into mscratch in
    // one swap: t0 still holds &pnm_save here, mscratch holds the old t0.
    // (A plain csrr would leave mscratch clobbered, breaking the next trap.)
    csrrw t0, mscratch, t0
    mret
