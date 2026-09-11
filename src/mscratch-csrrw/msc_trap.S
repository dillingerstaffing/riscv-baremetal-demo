// msc_trap.S: M-mode trap entry for the mscratch csrrw round-trip test.
//
// The spec for this module is zero traps. Any trap (which should be
// impossible here, since no faulting instruction is issued and no
// interrupts are enabled) increments msc_trapcount and parks the hart,
// so a run that took a trap would never reach its verdict lines. The
// handler deliberately never touches mscratch, leaving the CSR under
// test undisturbed.

    .section .text
    .global msc_trap_entry
    .align 2
msc_trap_entry:
    la t0, msc_trapcount
    ld t1, 0(t0)
    addi t1, t1, 1
    sd t1, 0(t0)
msc_halt:
    wfi
    j msc_halt
