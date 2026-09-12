    // tsr_trap.S: M-mode trap entry, phase drivers, and the S-mode
    // probe pad for the mstatus.TSR trap module.
    //
    // What this module proves: mstatus.TSR (Trap SRET, bit 22) is the
    // M-mode switch that forbids S-mode from executing sret. With
    // TSR=1, an S-mode sret raises an illegal-instruction trap
    // (mcause=0x2) delivered to M-mode with mepc exactly at the
    // sret; with TSR=0 the same sret returns to S-mode with no trap.
    //
    // tsr_m_trap_entry: M-mode vector. Saves every general-purpose
    // register on the current stack, calls the C handler
    // tsr_m_handler, restores, and mret. The C handler records the
    // trap (cause/epc/mtval), advances mepc by 4 past the expected
    // phase-2 sret trap, and redirects the phase-end ecall back to
    // the recorded M-mode resume label with MPP restored to
    // M-mode. Any other trap is reported and parks the hart inside
    // the C handler, so it shows up in the log as a FAIL.
    //
    // tsr_phase1 / tsr_phase2: M-mode phase drivers. Each arms sepc
    // with the shared S-mode probe pad, sets sstatus SPP=1 (the TSR
    // bit stays exactly as M-mode established it: clear in phase 1,
    // set in phase 2), records its own resume label address with an
    // in-asm label (never hardcoded) plus the ecall expectation flag,
    // then sret. When the pad ecalls back, the M-mode handler mrets
    // to the recorded resume label, which simply ret back to the C
    // driver.
    //
    // tsr_smode_pad: the S-mode probe sequence. Re-arms SPP=1 (the
    // driver's sret cleared SPP to U) and sepc to the post-sret
    // resume point, executes the probe sret, then issues one ecall
    // back to M-mode. The sret site and the ecall site are global
    // labels so the C driver can require mepc to equal each site
    // exactly. Both probe instructions are emitted under
    // .option norvc (4 bytes each), so the handler's fixed mepc+4
    // skip past the faulting sret always lands on the ecall.

    .section .text
    .global tsr_m_trap_entry
    .align 2
tsr_m_trap_entry:
    addi sp, sp, -240
    sd ra, 0(sp)
    sd gp, 8(sp)
    sd tp, 16(sp)
    sd t0, 24(sp)
    sd t1, 32(sp)
    sd t2, 40(sp)
    sd s0, 48(sp)
    sd s1, 56(sp)
    sd a0, 64(sp)
    sd a1, 72(sp)
    sd a2, 80(sp)
    sd a3, 88(sp)
    sd a4, 96(sp)
    sd a5, 104(sp)
    sd a6, 112(sp)
    sd a7, 120(sp)
    sd s2, 128(sp)
    sd s3, 136(sp)
    sd s4, 144(sp)
    sd s5, 152(sp)
    sd s6, 160(sp)
    sd s7, 168(sp)
    sd s8, 176(sp)
    sd s9, 184(sp)
    sd s10, 192(sp)
    sd s11, 200(sp)
    sd t3, 208(sp)
    sd t4, 216(sp)
    sd t5, 224(sp)
    sd t6, 232(sp)

    call tsr_m_handler

    ld ra, 0(sp)
    ld gp, 8(sp)
    ld tp, 16(sp)
    ld t0, 24(sp)
    ld t1, 32(sp)
    ld t2, 40(sp)
    ld s0, 48(sp)
    ld s1, 56(sp)
    ld a0, 64(sp)
    ld a1, 72(sp)
    ld a2, 80(sp)
    ld a3, 88(sp)
    ld a4, 96(sp)
    ld a5, 104(sp)
    ld a6, 112(sp)
    ld a7, 120(sp)
    ld s2, 128(sp)
    ld s3, 136(sp)
    ld s4, 144(sp)
    ld s5, 152(sp)
    ld s6, 160(sp)
    ld s7, 168(sp)
    ld s8, 176(sp)
    ld s9, 184(sp)
    ld s10, 192(sp)
    ld s11, 200(sp)
    ld t3, 208(sp)
    ld t4, 216(sp)
    ld t5, 224(sp)
    ld t6, 232(sp)
    addi sp, sp, 240
    mret

    .global tsr_phase1
    .align 2

tsr_phase1:
    la t0, tsr_smode_pad
    csrw sepc, t0
    csrr t0, sstatus
    ori t0, t0, 0x100          // SPP=1; TSR left as M-mode set it (clear)
    csrw sstatus, t0
    la t0, 1f
    la t1, tsr_resume_pc
    sd t0, 0(t1)
    li t0, 1
    la t1, tsr_expect_ecall
    sw t0, 0(t1)
    sret
1:
    ret

    .global tsr_phase2
    .align 2

tsr_phase2:
    la t0, tsr_smode_pad
    csrw sepc, t0
    csrr t0, sstatus
    ori t0, t0, 0x100          // SPP=1; TSR left as M-mode set it (set)
    csrw sstatus, t0
    la t0, 1f
    la t1, tsr_resume_pc
    sd t0, 0(t1)
    li t0, 2
    la t1, tsr_expect_ecall
    sw t0, 0(t1)
    sret
1:
    ret

    .global tsr_smode_pad
    .global tsr_sret_site
    .global tsr_ecall_site
    .global tsr_pad_lo
    .global tsr_pad_hi
    .align 2

tsr_pad_lo:
tsr_smode_pad:
    csrr t0, sstatus
    ori t0, t0, 0x100          // SPP=1 (the driver sret cleared it to U)
    csrw sstatus, t0
    la t0, 1f
    csrw sepc, t0
    .option norvc
tsr_sret_site:
    sret
1:
tsr_ecall_site:
    ecall
tsr_pad_hi:
    .option rvc
