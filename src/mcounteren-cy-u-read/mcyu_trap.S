// mcyu_trap.S: S-mode trap entry, M-mode trap entry, the S-mode
// ecall handoff, and the two U-mode payloads for the
// mcounteren-cy-u-read module.
//
// The S-mode entry: sscratch points at the 10-word mcyu_regs
// array in mcyu_main.c. The entry swaps t0 with sscratch, parks
// the interrupted s0 in s0 itself (callee-saved, so it survives
// the C dispatcher call below; t0/t1/a0 would not), then
// records scause, stval, the sepc value the hardware saved at
// trap entry, and bumps the trap counter (mcyu_regs[0..3]). It
// also records the interrupted t0 (parked in sscratch by the
// swap), the interrupted a0 (the phase signal register), the
// interrupted t1, and the interrupted s0 (mcyu_regs[5..8]).
//
// The entry then calls the C dispatcher mcyu_handle(&mcyu_regs),
// which appends the record to the trap history and returns the
// resume pc: sepc+4 for the illegal-instruction trap (the gated
// rdcycle), exact here because rdcycle has no compressed
// encoding, or an S-mode continuation for the U-mode ecall
// signals (setting sstatus.SPP=1 first, so sret resumes in
// S-mode instead of back in U-mode).
//
// Only the trap entry's documented registers are disturbed; the
// final symmetric csrrw restores the interrupted t0 and parks
// &mcyu_regs back in sscratch for the next trap.
//
// The M-mode entry expects exactly one trap: the S-mode ecall
// the phase-A continuation issues between the phases (S-mode
// ecalls are not delegated, so cause 9 lands here). It records
// mcause/mepc into mcyu_m_regs and calls the C handler
// mcyu_m_handle, which clears mcounteren.CY, verifies the
// readbacks, and mrets into the phase-B S-mode driver; it never
// returns to the entry. Any other M-mode trap is routed by the C
// handler to the FAIL path. The interrupted S-mode registers
// need no save/restore: the handler never resumes the
// interrupted context.
//
// The S-mode ecall handoff (mcyu_s_ecall) is a tiny labeled
// function so the M-mode handler can check mepc against
// mcyu_s_ecall_site exactly. The ret is unreachable: the M-mode
// handler mrets to the phase-B driver, never back here.

    .section .text
    .option push
    .option norvc
    .global mcyu_trap_entry
    .align 2                      // 4-byte aligned: stvec MODE=00 needs base[1:0] clear
mcyu_trap_entry:
    csrrw t0, sscratch, t0   // t0 = &mcyu_regs, interrupted t0 parked in sscratch
    sd s0, 8*8(t0)           // park interrupted s0; s0 now free as the regs pointer
    mv s0, t0                // s0 = &mcyu_regs (callee-saved: survives call)
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
    sd a0, 6*8(s0)           // interrupted a0 (phase signal / rdcycle dest region)
    mv a0, s0
    call mcyu_handle         // returns resume pc in a0; s0 intact (callee-saved)
    csrw sepc, a0            // resume where the dispatcher decided
    ld t1, 7*8(s0)           // restore interrupted t1
    mv t0, s0                // t0 = &mcyu_regs
    ld s0, 8*8(t0)           // restore interrupted s0
    csrrw t0, sscratch, t0   // symmetric swap: interrupted t0 back, sscratch=&regs
    sret
    .option pop

    .option push
    .option norvc
    .global mcyu_mt_trap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
mcyu_mt_trap_entry:
    csrr t0, mcause
    la t1, mcyu_m_regs
    sd t0, 0*8(t1)           // mcause at entry
    csrr t0, mepc
    sd t0, 1*8(t1)           // mepc at entry
    call mcyu_m_handle       // arms phase B and mrets; never returns
1:  wfi
    j 1b
    .option pop

    .option push
    .option norvc
    .global mcyu_s_ecall
    .global mcyu_s_ecall_site
    .align 2
mcyu_s_ecall:
mcyu_s_ecall_site:
    ecall
    ret                       // unreachable: M-mode mrets to the phase-B driver
    .option pop

// U-mode payload A: entered via sret with sstatus.SPP=0 while
// mcounteren.CY=1 and scounteren.CY=1. Both rdcycle reads must
// succeed. The second sample is re-read in a bounded spin until
// it differs from the first: back-to-back reads can land in the
// same host tick, and the verdict needs strictly increasing
// samples, so the loop waits for the tick. The bound (2^20
// iterations) is far beyond any real tick interval; on timeout
// the payload signals with SIG_A_TIMEOUT instead of SIG_A and
// S-mode reports the measured failure.
    .option push
    .option norvc
    .global mcyu_u_payload_a
    .global mcyu_u_rdcycle_a
    .global mcyu_u_ecall_a
    .align 2
mcyu_u_payload_a:
mcyu_u_rdcycle_a:
    rdcycle t0               // sample 0 -> interrupted t0 at the ecall
    li t2, (1 << 20)
1:  rdcycle t1               // sample 1 -> interrupted t1 at the ecall
    bne t1, t0, 2f
    addi t2, t2, -1
    bnez t2, 1b
    li a0, 0x9ae9            // MCYU_SIG_A_TIMEOUT: counter never advanced
    ecall
2:  li a0, 0x9a9a            // MCYU_SIG_A: phase A complete
mcyu_u_ecall_a:
    ecall
    .option pop

// U-mode payload B: entered via sret with sstatus.SPP=0 while
// mcounteren.CY=0 and scounteren.CY=1. The rdcycle must raise an
// illegal-instruction exception (delegated to S-mode); the
// handler skips it and resumes in U-mode at the li a0, which
// signals phase completion with an ecall (also delegated to
// S-mode).
    .option push
    .option norvc
    .global mcyu_u_payload_b
    .global mcyu_u_rdcycle_b
    .global mcyu_u_ecall_b
    .align 2
mcyu_u_payload_b:
    li t0, 0xdeadbeefdeadbeef
mcyu_u_rdcycle_b:
    rdcycle t0
    li a0, 0x9b9b            // MCYU_SIG_B: phase B complete
mcyu_u_ecall_b:
    ecall
    .option pop
