// scir_trap.S: S-mode trap entry, M-mode park handler, and the two
// U-mode payloads for the scounteren-ir-gate module.
//
// sscratch points at the 10-word scir_regs array in scir_main.c.
// The trap entry swaps t0 with sscratch, parks the interrupted s0
// in s0 itself (s0 is callee-saved, so it survives the C
// dispatcher call below; t0/t1/a0 would not), then records
// scause, stval, the sepc value the hardware saved at trap entry,
// and bumps the trap counter (scir_regs[0..3]). It also records
// the interrupted t0 (parked in sscratch by the swap), the
// interrupted a0 (the phase-complete signal register), the
// interrupted t1, and the interrupted s0 (scir_regs[5..8]).
//
// The entry then calls the C dispatcher scir_handle(&scir_regs),
// which appends the record to the trap history and returns the
// resume pc; the entry writes it to sepc. For the illegal-
// instruction trap (the gated rdinstret) the dispatcher returns
// sepc+4: exact here because rdinstret has no compressed encoding,
// so the faulting instruction is always 4 bytes. For the U-mode
// ecall (the phase-complete signal) the dispatcher returns an
// S-mode continuation address and sets sstatus.SPP=1 first, so
// sret resumes in S-mode instead of back in U-mode.
//
// Only the trap entry's documented registers are disturbed; the
// final symmetric csrrw restores the interrupted t0 and parks
// &scir_regs back in sscratch for the next trap.

    .section .text
    .option push
    .option norvc
    .global scir_trap_entry
    .align 2                      // 4-byte aligned: stvec MODE=00 needs base[1:0] clear
scir_trap_entry:
    csrrw t0, sscratch, t0   // t0 = &scir_regs, interrupted t0 parked in sscratch
    sd s0, 8*8(t0)           // park interrupted s0; s0 now free as the regs pointer
    mv s0, t0                // s0 = &scir_regs (callee-saved: survives call)
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
    sd a0, 6*8(s0)           // interrupted a0 (phase signal / rdinstret dest region)
    mv a0, s0
    call scir_handle         // returns resume pc in a0; s0 intact (callee-saved)
    csrw sepc, a0            // resume where the dispatcher decided
    ld t1, 7*8(s0)           // restore interrupted t1
    mv t0, s0                // t0 = &scir_regs
    ld s0, 8*8(t0)           // restore interrupted s0
    csrrw t0, sscratch, t0   // symmetric swap: interrupted t0 back, sscratch=&regs
    sret
    .option pop

// scir_mt_trap_entry: M-mode park handler. medeleg sends only the
// illegal-instruction trap and the U-mode ecall to S-mode; any
// trap that reaches M-mode is unexpected, so the hart parks and
// the bench harness observes the FAIL as a timeout (exit 124).
    .option push
    .option norvc
    .global scir_mt_trap_entry
    .align 2
scir_mt_trap_entry:
1:  wfi
    j 1b
    .option pop

// U-mode payload A: entered via sret with sstatus.SPP=0. With
// scounteren.IR clear the rdinstret must raise an illegal-
// instruction exception (delegated to S-mode); the handler skips
// it and resumes in U-mode at the li a0, which signals phase
// completion with an ecall (also delegated to S-mode).
    .option push
    .option norvc
    .global scir_u_payload_a
    .global scir_u_rdinstret_a
    .global scir_u_ecall_a
    .align 2
scir_u_payload_a:
    li t0, 0xdeadbeefdeadbeef
scir_u_rdinstret_a:
    rdinstret t0
    li a0, 0x9a9a            // SCIR_SIG_A: phase A complete
scir_u_ecall_a:
    ecall
    .option pop

// U-mode payload B: entered via sret with sstatus.SPP=0 after
// S-mode sets scounteren.IR. Both rdinstret reads must succeed.
// A known 16-instruction nop sequence separates the two reads.
// NOTE: on QEMU 8.2.2 without -icount the instret counter
// advances with host time (measured deltas ~12.5k-12.9k, varying
// run to run), not as a per-retired-instruction count, so the
// nop sequence does not explain the observed delta; the verdict
// is the honest "two reads, strictly increasing". The sequence
// is retained as designed.
    .option push
    .option norvc
    .global scir_u_payload_b
    .global scir_u_rdinstret_b
    .global scir_u_ecall_b
    .align 2
scir_u_payload_b:
scir_u_rdinstret_b:
    rdinstret t0             // sample 0 -> interrupted t0 at the ecall
    .rept 16
    nop                      // 16 retired instructions between the samples
    .endr
    rdinstret t1             // sample 1 -> interrupted t1 at the ecall
    li a0, 0x9b9b            // SCIR_SIG_B: phase B complete
scir_u_ecall_b:
    ecall
    .option pop
