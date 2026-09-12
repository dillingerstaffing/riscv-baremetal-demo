    // tvt_trap.S: M-mode trap entry, phase drivers, and the S-mode
    // probe pad for the mstatus.TVM trap module.
    //
    // What this module proves: mstatus.TVM (bit 20) is the M-mode
    // switch that keeps virtual-memory control in M-mode. With
    // TVM=1, an S-mode write to satp and an S-mode sfence.vma each
    // raise an illegal-instruction trap (mcause=0x2) with mepc at the
    // faulting instruction; with TVM=0 the same two operations
    // execute normally (the satp write of the Bare value reads back,
    // the fence completes, no trap fires).
    //
    // tvt_m_trap_entry: M-mode vector. Saves every general-purpose
    // register on the current stack, calls the C handler
    // tvt_m_handler, restores, and mret. The C handler records the
    // trap (cause/epc/mtval), advances mepc by 4 past the two expected
    // synchronous probe traps, and redirects the phase-end ecall back
    // to the recorded M-mode resume label with MPP restored to
    // M-mode. Any other trap is reported and parks the hart inside
    // the C handler, so it shows up in the log as a FAIL.
    //
    // tvt_phase1 / tvt_phase2: M-mode phase drivers. Each arms sepc
    // with the shared S-mode probe pad, sets sstatus SPP=1 (the TVM
    // bit stays exactly as M-mode established it: set in phase 1,
    // clear in phase 2), records its own resume label address with an
    // in-asm label (never hardcoded) plus the ecall expectation flag,
    // then sret. When the pad ecalls back, the M-mode handler mrets
    // to the recorded resume label, which simply ret back to the C
    // driver.
    //
    // tvt_smode_pad: the S-mode probe sequence. Writes 0 (the Bare
    // MODE value, so no translation is ever enabled) to satp, reads
    // satp back into t1 and stores the readback, executes sfence.vma,
    // then issues one ecall back to M-mode. The satp write site, the
    // satp read site, and the fence site are global labels so the C
    // driver can require mepc to equal each site exactly. Every probe
    // instruction is emitted under .option norvc, so the handler's
    // fixed mepc+4 skip always lands on the next probe instruction.

    .section .text
    .global tvt_m_trap_entry
    .align 2
tvt_m_trap_entry:
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

    call tvt_m_handler

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

    .global tvt_phase1
    .align 2

tvt_phase1:
    la t0, tvt_smode_pad
    csrw sepc, t0
    csrr t0, sstatus
    ori t0, t0, 0x100          // SPP=1; TVM left as M-mode set it (set)
    csrw sstatus, t0
    la t0, 1f
    la t1, tvt_resume_pc
    sd t0, 0(t1)
    li t0, 1
    la t1, tvt_expect_ecall
    sw t0, 0(t1)
    sret
1:
    ret

    .global tvt_phase2
    .align 2

tvt_phase2:
    la t0, tvt_smode_pad
    csrw sepc, t0
    csrr t0, sstatus
    ori t0, t0, 0x100          // SPP=1; TVM left as M-mode set it (clear)
    csrw sstatus, t0
    la t0, 1f
    la t1, tvt_resume_pc
    sd t0, 0(t1)
    li t0, 2
    la t1, tvt_expect_ecall
    sw t0, 0(t1)
    sret
1:
    ret

    .global tvt_smode_pad
    .global tvt_satp_site
    .global tvt_satp_read_site
    .global tvt_fence_site
    .global tvt_pad_lo
    .global tvt_pad_hi
    .align 2

tvt_pad_lo:
tvt_smode_pad:
    .option norvc
tvt_satp_site:
    csrw satp, zero
tvt_satp_read_site:
    csrr t1, satp
    la t0, tvt_satp_rb
    sd t1, 0(t0)
tvt_fence_site:
    sfence.vma
    ecall
tvt_pad_hi:
    .option rvc
