// mir_mtrap.S: M-mode trap entry for the mcounteren-ir-gate
// module.
//
// A correct run takes exactly one M-mode trap: the ecall from the
// S-mode phase-A payload (mcause = 9, because medeleg leaves the
// S-mode ecall undelegated). On that entry the handler records
// mcause/mepc in the 5-word mir_m_regs array, bumps the M-mode
// trap count, reopens the IR gate (mcounteren = 0x7, readback
// stored in mir_m_regs[3]), then mrets with MPP=01 into the
// phase-B S-mode payload at mir_smode_phase_b. Any other M-mode
// trap (wrong mcause) parks the hart; the bench harness runs QEMU
// under timeout, so an unexpected M-mode trap is observable as
// exit status 124 rather than a silent divergence.
//
// t1 and t2 are clobbered by design: the handler never resumes the
// interrupted context, it mrets into a fresh payload entry point,
// so there is nothing to restore them for. t0 is preserved through
// the mscratch swap.

    .section .text
    .option push
    .option norvc
    .global mir_mtrap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
mir_mtrap_entry:
    csrrw t0, mscratch, t0   // t0 = &mir_m_regs, interrupted t0 parked in mscratch
    csrr t1, mcause
    sd t1, 0*8(t0)
    csrr t1, mepc
    sd t1, 1*8(t0)           // mepc as saved by the hardware at trap entry
    ld t1, 2*8(t0)
    addi t1, t1, 1
    sd t1, 2*8(t0)           // m-trap count++
    // The only expected entry: ecall from S-mode, mcause = 9.
    csrr t1, mcause
    li t2, 9
    bne t1, t2, mir_mpark
    // Reopen the IR gate for the second drop to S-mode.
    li t1, 0x7               // CY|TM|IR
    csrw mcounteren, t1
    csrr t1, mcounteren
    sd t1, 3*8(t0)           // mcounteren readback after the reopen
    // mret into mir_smode_phase_b with MPP=01 (S-mode).
    la t1, mir_smode_phase_b
    csrw mepc, t1
    csrr t1, mstatus
    li t2, 0x1800
    not t2, t2
    and t1, t1, t2           // clear MPP bits 12:11
    li t2, 0x800
    or t1, t1, t2            // MPP = 01 (S-mode)
    csrw mstatus, t1
    csrr t1, mscratch
    sd t1, 4*8(t0)           // interrupted t0
    csrrw t0, mscratch, t0   // symmetric swap: interrupted t0 back
    mret
mir_mpark:
    wfi
    j mir_mpark
    .option pop
