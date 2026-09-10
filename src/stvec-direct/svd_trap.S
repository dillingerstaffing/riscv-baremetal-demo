// svd_trap.S: S-mode direct-mode trap entry for the stvec-direct module.
//
// The run's only trap is an S-mode ecall, delegated to S-mode via
// medeleg bit 9, so it is taken through stvec (MODE=00) to this entry
// at the stvec base. sscratch points at the 32-word svd_save array.
//
// The handler:
//   1. saves the caller-saved registers it will clobber (t0 via the
//      sscratch swap, then ra, t1-t6, a0-a7),
//   2. records scause, sepc, stval into C-visible globals and bumps
//      the trap counter,
//   3. prints the trap record with the measured numbers and checks:
//      exactly 1 trap, scause == 9 (environment call from S-mode),
//      sepc == the captured ecall address, stval == 0,
//   4. on PASS writes the virt test-device finisher (QEMU exits 0);
//      on FAIL parks the hart in a wfi loop.
//
// There is no trap return: one trap per run is the whole experiment.

    .section .text
    .global svd_trap_entry
    .align 2                      // 4-byte aligned: stvec MODE=00 needs base[1:0] clear
svd_trap_entry:
    csrrw t0, sscratch, t0        // t0 = &svd_save; interrupted t0 parked in sscratch
    sd x1, 1*8(t0)                // ra
    sd x6, 6*8(t0)                // t1
    sd x7, 7*8(t0)                // t2
    sd x10, 10*8(t0)              // a0
    sd x11, 11*8(t0)
    sd x12, 12*8(t0)
    sd x13, 13*8(t0)
    sd x14, 14*8(t0)
    sd x15, 15*8(t0)
    sd x16, 16*8(t0)
    sd x17, 17*8(t0)              // a7
    sd x28, 28*8(t0)              // t3
    sd x29, 29*8(t0)
    sd x30, 30*8(t0)
    sd x31, 31*8(t0)              // t6
    csrr t1, sscratch             // the interrupted t0
    sd t1, 5*8(t0)

    // Record scause/sepc/stval and the trap count in C-visible globals.
    csrr t1, scause
    la t2, svd_scause
    sd t1, 0(t2)
    csrr t1, sepc
    la t2, svd_sepc
    sd t1, 0(t2)
    csrr t1, stval
    la t2, svd_stval
    sd t1, 0(t2)
    la t2, svd_traps
    ld t1, 0(t2)
    addi t1, t1, 1
    sd t1, 0(t2)

    // Load the record into s0-s4 (callee-saved: uart calls cannot
    // disturb them) for printing and checking.
    la t1, svd_scause
    ld s0, 0(t1)
    la t1, svd_sepc
    ld s1, 0(t1)
    la t1, svd_stval
    ld s2, 0(t1)
    la t1, svd_ecall_addr
    ld s3, 0(t1)
    la t1, svd_traps
    ld s4, 0(t1)
    li s5, 0                       // fail flag

    la a0, msg_hdr
    call uart_puts

    la a0, msg_scause
    call uart_puts
    mv a0, s0
    call uart_put_hex
    la a0, msg_nl
    call uart_puts

    la a0, msg_sepc
    call uart_puts
    mv a0, s1
    call uart_put_hex
    la a0, msg_nl
    call uart_puts

    la a0, msg_stval
    call uart_puts
    mv a0, s2
    call uart_put_hex
    la a0, msg_nl
    call uart_puts

    la a0, msg_eaddr
    call uart_puts
    mv a0, s3
    call uart_put_hex
    la a0, msg_nl
    call uart_puts

    la a0, msg_traps
    call uart_puts
    mv a0, s4
    call uart_put_dec
    la a0, msg_nl
    call uart_puts

    // Check 1: exactly one trap this run.
    li t1, 1
    beq s4, t1, chk_cause
    la a0, msg_f_traps
    call uart_puts
    li s5, 1
chk_cause:
    // Check 2: scause == 9 (environment call from S-mode).
    li t1, 9
    beq s0, t1, chk_epc
    la a0, msg_f_scause
    call uart_puts
    li s5, 1
chk_epc:
    // Check 3: sepc == the captured ecall address, exactly.
    beq s1, s3, chk_tval
    la a0, msg_f_sepc
    call uart_puts
    li s5, 1
chk_tval:
    // Check 4: stval == 0 (ecall traps carry no faulting address).
    beqz s2, verdict
    la a0, msg_f_stval
    call uart_puts
    li s5, 1
verdict:
    bnez s5, do_fail
    la a0, msg_pass
    call uart_puts
    li t1, 0x100000                // virt test-device finisher
    li t2, 0x5555
    sw t2, 0(t1)                    // PASS: QEMU exits 0
    j park                          // unreachable, safety net
do_fail:
    la a0, msg_fail
    call uart_puts
park:
    wfi
    j park

    .section .rodata
msg_hdr:    .string "\n--- trap record (S-mode, via stvec direct) ---\n"
msg_scause: .string "  scause     = "
msg_sepc:   .string "  sepc       = "
msg_stval:  .string "  stval      = "
msg_eaddr:  .string "  ecall_addr = "
msg_traps:  .string "  traps      = "
msg_nl:     .string "\n"
msg_f_traps:  .string "  FAIL: trap count != 1\n"
msg_f_scause: .string "  FAIL: scause != 9\n"
msg_f_sepc:   .string "  FAIL: sepc != ecall_addr\n"
msg_f_stval:  .string "  FAIL: stval != 0\n"
msg_pass:   .string "RESULT: PASS\n"
msg_fail:   .string "RESULT: FAIL\n"
