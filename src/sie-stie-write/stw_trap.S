    // stw_trap.S: M-mode trap entry for the sie WARL probe module.
    //
    // No trap is expected in this module: MIE stays clear and no
    // interrupt source is armed. The entry counts the trap into
    // stw_trap_count and parks the hart without returning; the bench
    // harness runs QEMU under `timeout`, so a parked hart is
    // observable as the timeout exit status, i.e. FAIL. Reaching the
    // verdict line in the log therefore proves the counter stayed 0.

    .section .text
    .global stw_trap_entry
    .align 2
stw_trap_entry:
    la t0, stw_trap_count
    ld t1, 0(t0)
    addi t1, t1, 1
    sd t1, 0(t0)
ssw_park:
    wfi
    j ssw_park
