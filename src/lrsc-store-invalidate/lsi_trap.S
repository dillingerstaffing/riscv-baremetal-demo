// lsi_trap.S: M-mode trap entry for the lrsc-store-invalidate module.
//
// mscratch points at the 4-word lsi_save array in lsi_main.c. On
// entry the handler swaps t0 with mscratch, records
// mcause/mepc/mtval into the array, flags the trap as seen, restores
// t0 (putting &lsi_save back into mscratch), and parks the hart. No
// trap is expected in this experiment: lr.w, sw, and sc.w are all
// legal on aligned words, so any trap means the environment, not
// the reservation, is at fault. The seen flag is checked as part of
// the verdict, so an unexpected trap fails the run instead of
// passing silently.

    .section .text
    .global lsi_trap_entry
    .align 2
lsi_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &lsi_save, old t0 parked in mscratch
    csrr t1, mcause
    sd t1, 0(t0)             // save[0]: mcause
    csrr t1, mepc
    sd t1, 8(t0)             // save[1]: mepc
    csrr t1, mtval
    sd t1, 16(t0)            // save[2]: mtval
    li t1, 1
    sd t1, 24(t0)            // save[3]: seen flag
    // Restore the interrupted t0 and put &lsi_save back into
    // mscratch in one swap: t0 still holds &lsi_save here,
    // mscratch holds the old t0. t1 is left clobbered; the hart
    // never resumes.
    csrrw t0, mscratch, t0
halt_loop:
    wfi
    j halt_loop
