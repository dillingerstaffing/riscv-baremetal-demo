// warl_trap.S: M-mode trap entry for the satp-mode-warl module.
//
// No traps are expected on the happy path: the only satp write that
// enables translation (MODE=8, Sv39) uses a hand-verified table whose
// two megapage leaves identity-map the code/data window and the UART
// window, and the MODE=9/10/15 probes are specified to take no effect
// on this hart (readback must equal the pre-write Bare value), so no
// fault can come from the translation machinery. satp itself is a
// legal CSR to read and write in S-mode, so the write/readback
// sequence cannot trap either.
//
// If a trap does fire, this handler records mcause/mepc/mtval into
// C-visible globals, bumps the trap counter, prints a FAIL record,
// and parks the hart in a wfi loop. A run that reaches the completion
// marker in warl_main.c therefore verified trap count == 0.
//
// mscratch points at the 32-word warl_save array while the module
// runs; mtvec holds this entry (MODE=00, direct).

    .section .text
    .global warl_trap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
warl_trap_entry:
    csrrw t0, mscratch, t0        // t0 = &warl_save; interrupted t0 parked in mscratch
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
    sd x29, 29*8(t0)              // t4
    sd x30, 30*8(t0)              // t5
    sd x31, 31*8(t0)              // t6
    csrr t1, mscratch             // the interrupted t0
    sd t1, 5*8(t0)

    // Record mcause/mepc/mtval and the trap count in C-visible globals.
    csrr t1, mcause
    la t2, warl_mcause
    sd t1, 0(t2)
    csrr t1, mepc
    la t2, warl_mepc
    sd t1, 0(t2)
    csrr t1, mtval
    la t2, warl_mtval
    sd t1, 0(t2)
    la t2, warl_traps
    ld t1, 0(t2)
    addi t1, t1, 1
    sd t1, 0(t2)

    // Load the record into s0-s3 (callee-saved: uart calls cannot
    // disturb them) for printing.
    la t1, warl_mcause
    ld s0, 0(t1)
    la t1, warl_mepc
    ld s1, 0(t1)
    la t1, warl_mtval
    ld s2, 0(t1)
    la t1, warl_traps
    ld s3, 0(t1)

    la a0, msg_hdr
    call uart_puts

    la a0, msg_mcause
    call uart_puts
    mv a0, s0
    call uart_put_hex
    la a0, msg_nl
    call uart_puts

    la a0, msg_mepc
    call uart_puts
    mv a0, s1
    call uart_put_hex
    la a0, msg_nl
    call uart_puts

    la a0, msg_mtval
    call uart_puts
    mv a0, s2
    call uart_put_hex
    la a0, msg_nl
    call uart_puts

    la a0, msg_traps
    call uart_puts
    mv a0, s3
    call uart_put_dec
    la a0, msg_nl
    call uart_puts

    // Any trap at all is a failure for this module: print the
    // verdict and park the hart.
    la a0, msg_fail
    call uart_puts
park:
    wfi
    j park

    .section .rodata
msg_hdr:    .string "\n--- unexpected trap record (M-mode, via mtvec direct) ---\n"
msg_mcause: .string "  mcause = "
msg_mepc:   .string "  mepc   = "
msg_mtval:  .string "  mtval  = "
msg_traps:  .string "  traps  = "
msg_nl:     .string "\n"
msg_fail:   .string "RESULT: FAIL (unexpected trap)\n"
