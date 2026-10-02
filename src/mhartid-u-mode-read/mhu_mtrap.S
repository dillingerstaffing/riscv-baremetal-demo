// mhu_mtrap.S: M-mode trap entry for the mhartid-u-mode-read module.
//
// The only M-mode trap the run plans is the U-mode ecall that
// ends the payload (mcause 8: environment call from U-mode).
// The ecall is not delegated (only medeleg bit 2, illegal
// instruction, is set), so it lands here. On that path the
// handler restores medeleg and pmpcfg0 to their boot values
// saved by main in mhu_boot_medeleg / mhu_boot_pmpcfg0, writes
// the virt test-device finisher word 0x5555 when mhu_result is
// PASS, and parks the hart. Any other M-mode trap is
// unexpected: the handler bumps mhu_mt_traps and parks, so the
// bench harness observes it as a timeout and the run fails
// loudly instead of being silently absorbed.

    .section .text
    .option push
    .option norvc
    .global mhu_mt_trap_entry
    .align 2
mhu_mt_trap_entry:
    csrr t1, mcause
    li t0, 8
    bne t1, t0, mhu_mt_unexpected
    // Planned exit: restore the CSRs main delegated/masked to
    // their boot values before the machine shuts down.
    la t0, mhu_boot_medeleg
    ld t1, 0(t0)
    csrw medeleg, t1
    la t0, mhu_boot_pmpcfg0
    ld t1, 0(t0)
    csrw pmpcfg0, t1
    la t0, mhu_result
    ld t1, 0(t0)
    beqz t1, mhu_mt_park       // FAIL: no finisher word
    li t0, 0x100000
    li t1, 0x5555
    sw t1, 0(t0)               // PASS: virt test-device exit
mhu_mt_park:
    wfi
    j mhu_mt_park
mhu_mt_unexpected:
    la t0, mhu_mt_traps
    ld t1, 0(t0)
    addi t1, t1, 1
    sd t1, 0(t0)
    j mhu_mt_park
    .option pop
