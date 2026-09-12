    // mmsw_trap.S: M-mode trap entry for the mip.MSIP software-write probe.
    //
    // No trap is expected in this module: mstatus.MIE stays clear and
    // mie.MSIE stays clear for the whole run, so the pended machine
    // software interrupt must remain undelivered. The entry counts the
    // trap into mmsw_trap_count and parks the hart without returning;
    // the bench harness runs QEMU under `timeout`, so a parked hart
    // is observable as the timeout exit status, i.e. FAIL. Reaching
    // the verdict line in the log therefore proves the counter stayed
    // 0.

    .section .text
    .global mmsw_trap_entry
    .align 2
mmsw_trap_entry:
    la t0, mmsw_trap_count
    ld t1, 0(t0)
    addi t1, t1, 1
    sd t1, 0(t0)
mmsw_park:
    wfi
    j mmsw_park
