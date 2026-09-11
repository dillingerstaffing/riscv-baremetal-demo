// sepc_trap.S: M-mode trap entry for the sepc WARL module.
//
// mscratch points at the 8-word sep_regs array in sepc_main.c.
// On entry the handler saves t0 through mscratch, then records
// mcause, sepc, and mepc, and calls the C handler sep_c_handle(),
// which bumps the trap counter, records the cause, sepc, and mepc,
// and (for the deliberate ecall, mcause = 11) advances the mepc CSR
// past the 4-byte ecall so the run continues. For any other trap
// cause the C handler parks the hart instead of returning; that
// path is a FAIL, observable as the harness timeout. Everything the
// interrupted code needs is restored before mret; ra/a0 are saved
// because the C call clobbers them. The interrupted code in this
// module only spins on volatile counters, so the minimal save set
// is sufficient.

    .section .text
    .global sepc_trap_entry
    .align 2
sepc_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &sep_regs, interrupted t0 parked in mscratch
    sd t1, 1*8(t0)
    csrr t1, mcause
    sd t1, 2*8(t0)
    csrr t1, sepc
    sd t1, 3*8(t0)
    csrr t1, mepc
    sd t1, 6*8(t0)
    sd ra, 4*8(t0)
    sd a0, 5*8(t0)
    call sep_c_handle        // counts the trap, records cause/sepc/mepc
    ld ra, 4*8(t0)
    ld a0, 5*8(t0)
    ld t1, 1*8(t0)           // restore the interrupted t1
    // One swap restores the interrupted t0 and parks &sep_regs back
    // in mscratch for the next trap.
    csrrw t0, mscratch, t0
    mret
