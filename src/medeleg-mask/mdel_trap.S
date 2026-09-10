// mdel_trap.S: M-mode trap entry for the medeleg writable-mask module.
//
// mscratch points at the 8-word mdel_regs array in mdel_main.c.
// On entry the handler bumps the trap counter, records mcause, mepc,
// and mtval, advances mepc by 4 (skipping the ecall that caused the
// trap), then restores the interrupted state and returns with mret.
// Only t0 and t1 are used, both restored.

    .section .text
    .global mdel_trap_entry
    .align 2
mdel_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &mdel_regs, interrupted t0 parked in mscratch
    sd t1, 1*8(t0)           // park interrupted t1 in slot 1
    ld t1, 0*8(t0)           // bump the trap counter in slot 0
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, mcause
    sd t1, 2*8(t0)           // slot 2: mcause of this trap
    csrr t1, mepc
    sd t1, 3*8(t0)           // slot 3: mepc of this trap
    csrr t1, mtval
    sd t1, 4*8(t0)           // slot 4: mtval of this trap
    csrr t1, mepc            // skip the trapping ecall on resume
    addi t1, t1, 4
    csrw mepc, t1
    ld t1, 1*8(t0)           // restore the interrupted t1
    // One swap restores the interrupted t0 and parks &mdel_regs back
    // in mscratch for the next trap.
    csrrw t0, mscratch, t0
    mret
