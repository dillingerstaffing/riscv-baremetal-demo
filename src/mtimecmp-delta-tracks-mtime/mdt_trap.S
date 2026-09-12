// mdt_trap.S: minimal M-mode trap handler for the
// mtimecmp-delta-tracks-mtime module.
//
// Any trap during this experiment is unexpected: the program only
// issues plain MMIO loads to the CLINT mtime register in M-mode,
// with interrupts left disabled (mie and mstatus.MIE are 0 at
// reset), so the handler records mcause/mepc/mtval and a trap
// count into mdt_save (in mdt_main.c) and parks the hart in a wfi
// loop. The main program prints the trap count at the end and
// requires it to be zero, so a printed PASS implies no trap
// fired; a FAIL shows up as a timeout (exit 124) under the bench
// harness.

    .section .text
    .option push
    .option norvc
    .global mdt_trap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
mdt_trap_entry:
    la t0, mdt_save
    sd t1, 0*8(t0)           // park interrupted t1
    csrr t1, mcause
    sd t1, 1*8(t0)
    csrr t1, mepc
    sd t1, 2*8(t0)
    csrr t1, mtval
    sd t1, 3*8(t0)
    ld t1, 4*8(t0)
    addi t1, t1, 1
    sd t1, 4*8(t0)           // trap_count++
    // No resume: any trap is a failure. Park the hart.
1:  wfi
    j 1b
    .option pop
