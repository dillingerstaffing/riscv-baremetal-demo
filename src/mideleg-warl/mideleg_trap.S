// mideleg_trap.S: M-mode trap entry for the mideleg WARL module.
//
// This module deliberately causes no traps: MIE stays clear and no
// interrupt source is armed. The entry is defensive only. Any
// unexpected trap parks the hart here without ever returning; the
// bench harness runs QEMU under `timeout`, so a parked hart is
// observable as the timeout exit status, i.e. FAIL.

    .section .text
    .global mideleg_trap_entry
    .align 2
mideleg_trap_entry:
    wfi
    j mideleg_trap_entry
