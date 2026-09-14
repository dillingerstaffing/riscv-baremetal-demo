// stv_mtrap.S: M-mode trap entry for the stval-warl-probe
// module.
//
// The experiment expects exactly one M-mode trap: an ecall from
// S-mode issued after the S-mode payload finishes. On that ecall
// the handler records mcause/mepc and the trap count in the
// 3-word stv_m_regs array (mscratch points at it), advances mepc
// past the ecall, and calls the C verdict routine stv_ecall_done
// (in stv_main.c), which prints the checksum and RESULT and then
// either shuts the machine down through the virt test-device
// finisher (PASS) or parks the hart in wfi (FAIL). The verdict
// routine never returns, so this handler never mret.
//
// Any other M-mode trap (mcause != 9) parks the hart; the bench
// harness runs QEMU under timeout, so an unexpected M-mode trap
// is observable as exit status 124 rather than a silent
// divergence.
//
// mscratch points at stv_m_regs: [0] mcause, [1] mepc at entry,
// [2] M-mode trap count. t0 is scratch-swapped and restored;
// t1/t2 are used unsaved because the taken path calls the C
// verdict routine, which never returns.

    .section .text
    .option push
    .option norvc
    .global stv_mtrap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
stv_mtrap_entry:
    csrrw t0, mscratch, t0   // t0 = &stv_m_regs, interrupted t0 parked in mscratch
    // t1/t2 are used unsaved below: the only taken path calls the
    // C verdict routine, which never returns, so the interrupted
    // context (S-mode, just past its final ecall) never resumes
    // and the clobber is unobservable.
    csrr t1, mcause
    li t2, 9                // ecall from S-mode
    bne t1, t2, stv_mpark
    sd t1, 0*8(t0)          // mcause (== 9)
    csrr t1, mepc
    sd t1, 1*8(t0)          // mepc at the ecall
    ld t2, 2*8(t0)
    addi t2, t2, 1
    sd t2, 2*8(t0)          // M-mode trap count++
    csrr t2, mepc
    addi t2, t2, 4
    csrw mepc, t2           // mepc past the ecall (kept exact; the
                            // verdict routine never returns)
    csrrw t0, mscratch, t0   // symmetric swap: interrupted t0 back
    call stv_ecall_done     // never returns
    // Unreachable; belt and braces.
stv_mpark:
    wfi
    j stv_mpark
    .option pop
