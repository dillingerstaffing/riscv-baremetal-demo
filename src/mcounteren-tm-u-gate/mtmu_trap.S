// mtmu_trap.S: S-mode trap entry, M-mode trap entry, the S-mode
// ecall handoff, and the three U-mode payloads for the
// mcounteren-tm-u-gate module.
//
// The S-mode entry: sscratch points at the 10-word mtmu_regs
// array in mtmu_main.c. The entry swaps t0 with sscratch, parks
// the interrupted s0 in s0 itself (callee-saved, so it survives
// the C dispatcher call below; t0/t1/a0 would not), then
// records scause, stval, the sepc value the hardware saved at
// trap entry, and bumps the trap counter (mtmu_regs[0..3]). It
// also records the interrupted t0 (parked in sscratch by the
// swap), the interrupted a0 (the phase signal register), the
// interrupted t1, and the interrupted s0 (mtmu_regs[5..8]).
//
// The entry then calls the C dispatcher mtmu_handle(&mtmu_regs),
// which appends the record to the trap history and returns the
// resume pc: sepc+4 for the illegal-instruction trap (the gated
// rdtime in phase B), exact here because rdtime has no compressed
// encoding, or an S-mode continuation for the U-mode ecall
// signals (setting sstatus.SPP=1 first, so sret resumes in
// S-mode instead of back in U-mode).
//
// Only the trap entry's documented registers are disturbed; the
// final symmetric csrrw restores the interrupted t0 and parks
// &mtmu_regs back in sscratch for the next trap.
//
// The M-mode entry expects exactly three traps, in order: the
// S-mode ecalls the phase-A, phase-B, and phase-C continuations
// issue as handoffs (S-mode ecalls are not delegated, so cause 9
// lands here). It records mcause/mepc into mtmu_m_regs and bumps
// the M-mode trap counter (mtmu_m_regs[2]), then calls the C
// handler mtmu_m_handle, which arms the next phase or restores
// the CSRs and exits; it never returns to the entry. Any other
// M-mode trap is routed by the C handler to the FAIL path. The
// interrupted S-mode registers need no save/restore: the handler
// never resumes the interrupted context.
//
// The S-mode ecall handoff (mtmu_s_ecall) is a tiny labeled
// function so the M-mode handler can check mepc against
// mtmu_s_ecall_site exactly. The ret is unreachable: the M-mode
// handler mrets to the next phase driver, never back here.

    .section .text
    .option push
    .option norvc
    .global mtmu_trap_entry
    .align 2                      // 4-byte aligned: stvec MODE=00 needs base[1:0] clear
mtmu_trap_entry:
    csrrw t0, sscratch, t0   // t0 = &mtmu_regs, interrupted t0 parked in sscratch
    sd s0, 8*8(t0)           // park interrupted s0; s0 now free as the regs pointer
    mv s0, t0                // s0 = &mtmu_regs (callee-saved: survives call)
    sd t1, 7*8(s0)           // park interrupted t1
    csrr t1, scause
    sd t1, 0*8(s0)
    csrr t1, stval
    sd t1, 1*8(s0)
    csrr t1, sepc
    sd t1, 2*8(s0)           // sepc as saved by the hardware at trap entry
    ld t1, 3*8(s0)
    addi t1, t1, 1
    sd t1, 3*8(s0)           // trap_count++
    csrr t1, sscratch
    sd t1, 5*8(s0)           // interrupted t0
    sd a0, 6*8(s0)           // interrupted a0 (phase signal / rdtime dest region)
    mv a0, s0
    call mtmu_handle         // returns resume pc in a0; s0 intact (callee-saved)
    csrw sepc, a0            // resume where the dispatcher decided
    ld t1, 7*8(s0)           // restore interrupted t1
    mv t0, s0                // t0 = &mtmu_regs
    ld s0, 8*8(t0)           // restore interrupted s0
    csrrw t0, sscratch, t0   // symmetric swap: interrupted t0 back, sscratch=&regs
    sret
    .option pop

    .option push
    .option norvc
    .global mtmu_mt_trap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
mtmu_mt_trap_entry:
    csrr t0, mcause
    la t1, mtmu_m_regs
    sd t0, 0*8(t1)           // mcause at entry
    csrr t0, mepc
    sd t0, 1*8(t1)           // mepc at entry
    ld t0, 2*8(t1)
    addi t0, t0, 1
    sd t0, 2*8(t1)           // M-mode trap_count++
    call mtmu_m_handle       // arms the next phase or restores and exits; never returns
1:  wfi
    j 1b
    .option pop

    .option push
    .option norvc
    .global mtmu_s_ecall
    .global mtmu_s_ecall_site
    .align 2
mtmu_s_ecall:
mtmu_s_ecall_site:
    ecall
    ret                       // unreachable: M-mode mrets to the next phase driver
    .option pop

// U-mode payload A (control): entered via sret with sstatus.SPP=0
// while mcounteren.TM=1 and scounteren.TM=1. Both rdtime reads
// must succeed. The second sample is re-read in a bounded spin
// until it differs from the first: back-to-back reads can land
// in the same host tick, and the verdict needs strictly
// increasing samples, so the loop waits for the tick. The bound
// (2^20 iterations) is far beyond any real tick interval; on
// timeout the payload signals with SIG_A_TIMEOUT instead of
// SIG_A and S-mode reports the measured failure.
    .option push
    .option norvc
    .global mtmu_u_payload_a
    .global mtmu_u_rdtime_a
    .global mtmu_u_ecall_a
    .align 2
mtmu_u_payload_a:
mtmu_u_rdtime_a:
    rdtime t0                // sample 0 -> interrupted t0 at the ecall
    li t2, (1 << 20)
1:  rdtime t1                // sample 1 -> interrupted t1 at the ecall
    bne t1, t0, 2f
    addi t2, t2, -1
    bnez t2, 1b
    li a0, 0xa1e1            // MTMU_SIG_A_TIMEOUT: timer never advanced
    ecall
2:  li a0, 0xa1a1            // MTMU_SIG_A: phase A complete
mtmu_u_ecall_a:
    ecall
    .option pop

// U-mode payload B (gate): entered via sret with sstatus.SPP=0
// while mcounteren.TM=0 and scounteren.TM=1. The rdtime must
// raise an illegal-instruction exception (delegated to S-mode);
// the handler skips it and resumes in U-mode at the li a0, which
// signals phase completion with an ecall (also delegated to
// S-mode).
    .option push
    .option norvc
    .global mtmu_u_payload_b
    .global mtmu_u_rdtime_b
    .global mtmu_u_ecall_b
    .align 2
mtmu_u_payload_b:
    li t0, 0xdeadbeefdeadbeef
mtmu_u_rdtime_b:
    rdtime t0
    li a0, 0xa2a2            // MTMU_SIG_B: phase B complete
mtmu_u_ecall_b:
    ecall
    .option pop

// U-mode payload C (restore): entered via sret with sstatus.SPP=0
// while mcounteren.TM=1 again and scounteren.TM=1. Both rdtime
// reads must succeed, same bounded-spin discipline as payload A.
// On timeout the payload signals with SIG_C_TIMEOUT instead of
// SIG_C and S-mode reports the measured failure.
    .option push
    .option norvc
    .global mtmu_u_payload_c
    .global mtmu_u_rdtime_c
    .global mtmu_u_ecall_c
    .align 2
mtmu_u_payload_c:
mtmu_u_rdtime_c:
    rdtime t0                // sample 0 -> interrupted t0 at the ecall
    li t2, (1 << 20)
1:  rdtime t1                // sample 1 -> interrupted t1 at the ecall
    bne t1, t0, 2f
    addi t2, t2, -1
    bnez t2, 1b
    li a0, 0xa3e1            // MTMU_SIG_C_TIMEOUT: timer never advanced
    ecall
2:  li a0, 0xa3a3            // MTMU_SIG_C: phase C complete
mtmu_u_ecall_c:
    ecall
    .option pop
