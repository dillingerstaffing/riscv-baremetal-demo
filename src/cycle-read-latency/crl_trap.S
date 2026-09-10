// crl_trap.S: minimal M-mode trap entry for the rdcycle
// read-latency module.
//
// No trap is expected: the module is 27 back-to-back CSR reads and
// plain loads and stores. mscratch points at the 4-word
// crl_trap_rec array in crl_main.c. On entry the handler saves t0
// through mscratch, records mcause/mepc/mtval, bumps
// crl_trap_count, and parks the hart in a wfi loop without
// returning. A trap is therefore observable as a missing RESULT
// line plus the harness timeout exit status (124).

    .section .text
    .global crl_trap_entry
    .align 2
crl_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &crl_trap_rec, interrupted t0 parked in mscratch
    sd t1, 3*8(t0)
    csrr t1, mcause
    sd t1, 0*8(t0)
    csrr t1, mepc
    sd t1, 1*8(t0)
    csrr t1, mtval
    sd t1, 2*8(t0)
    ld t1, 3*8(t0)           // restore the interrupted t1
    csrrw t0, mscratch, t0    // restore the interrupted t0
    la t0, crl_trap_count
    ld t1, 0(t0)
    addi t1, t1, 1
    sd t1, 0(t0)
1:  wfi
    j 1b
