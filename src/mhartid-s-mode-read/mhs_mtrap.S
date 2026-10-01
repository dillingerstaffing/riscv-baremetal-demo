// mhs_mtrap.S: M-mode trap entry for the mhartid-s-mode-read module.
//
// No M-mode trap is expected in a correct run: the illegal
// instruction raised by the S-mode mhartid read is delegated to
// S-mode through medeleg bit 2, and the S-mode payload finishes
// through the test-device finisher, not through an ecall. The
// handler therefore only counts the trap in mhs_mt_traps and
// parks the hart; the bench harness observes the park as a
// timeout, so an unexpected M-mode trap fails the run instead of
// being silently absorbed.

    .section .text
    .option push
    .option norvc
    .global mhs_mt_trap_entry
    .align 2
mhs_mt_trap_entry:
    la t0, mhs_mt_traps
    ld t1, 0(t0)
    addi t1, t1, 1
    sd t1, 0(t0)
mhs_mt_park:
    wfi
    j mhs_mt_park
    .option pop
