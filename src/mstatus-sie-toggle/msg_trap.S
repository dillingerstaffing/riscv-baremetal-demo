    // msg_trap.S: trap entries, phase drivers, and the S-mode landing pad
    // for the mstatus.SIE gate module.
    //
    // What this module proves: the S-mode interrupt gate bit (bit 1,
    // addressable as mstatus.SIE from M-mode and as sstatus.SIE from
    // S-mode; the privileged spec defines sstatus as a subset view of
    // mstatus, so the two names are one physical bit) is under
    // M-mode's control. M-mode clears it through the mstatus CSR and a
    // pending delegated supervisor software interrupt does not trap
    // across an S-mode window; M-mode sets it through mstatus and the
    // same pending interrupt traps exactly once in S-mode. The C
    // driver verifies the aliasing directly: every mstatus.SIE write
    // is read back through both mstatus and sstatus.
    //
    // Siblings: sie-stie-gate tests a different bit (the sie.STIE
    // per-interrupt enable); sstatus-sie-gate tests the same gate bit
    // driven from S-mode. This module drives it from M-mode.
    //
    // msg_m_trap_entry: M-mode vector. Saves every general-purpose
    // register on the stack, calls the C handler msg_m_handler,
    // restores, and mret. The C handler redirects mepc for the two
    // expected S-mode ecalls so mret lands back in the phase driver
    // below; any other M-mode trap is reported and parks the hart
    // inside the C handler, so it shows up in the log as a FAIL.
    //
    // msg_s_trap_entry: S-mode vector. Swaps t0 with sscratch so t0
    // points at msg_save, parks the interrupted t1, saves every
    // general-purpose register, then calls the C handler msg_s_handler
    // on a dedicated trap stack. The handler takes the one expected
    // supervisor software trap, records scause/sepc and the cycle
    // counter, and clears the pending SSIP bit so the trap cannot
    // re-fire. Exit restores every register, reinstalls msg_save in
    // sscratch, and sret resumes the interrupted code with sepc
    // unchanged.
    //
    // msg_phase1 / msg_phase2: M-mode phase drivers. Each arms sepc
    // with the landing pad address, sets sstatus SPP (leaving the SIE
    // bit exactly as M-mode established it through mstatus: clear in
    // phase 1, set in phase 2), points sscratch at msg_save, records
    // its own resume label address (computed here with an in-asm
    // label, never hardcoded) plus the ecall expectation flag, then
    // sret. When the S-mode code ecalls back, the M-mode handler mrets
    // to the recorded resume label, which simply ret back to the C
    // driver. The C driver therefore runs strictly in order: phase-1
    // driver, phase-1 checks, phase-2 driver, phase-2 checks.
    //
    // smode_landing: the S-mode entry point. Spins on rdcycle for a
    // bounded cycle window, then issues one ecall back to M-mode. In
    // phase 1 the supervisor software interrupt is pending but the
    // gate is closed, so the whole window elapses with no trap. In
    // phase 2 the gate is open: the pending interrupt traps exactly
    // once at (or just after) this pad, the handler clears the pending
    // bit, sret resumes the spin, and the rest of the window is the
    // quiet window.

    .section .text
    .global msg_m_trap_entry
    .align 2

msg_m_trap_entry:
    addi sp, sp, -256
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

    call msg_m_handler

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
    addi sp, sp, 256
    mret

    .global msg_s_trap_entry
    .align 2

msg_s_trap_entry:
    csrrw t0, sscratch, t0
    sd t1, 8(t0)

    sd ra, 32(t0)
    sd sp, 40(t0)
    sd gp, 48(t0)
    sd tp, 56(t0)
    sd t2, 64(t0)
    sd s0, 72(t0)
    sd s1, 80(t0)
    sd a0, 88(t0)
    sd a1, 96(t0)
    sd a2, 104(t0)
    sd a3, 112(t0)
    sd a4, 120(t0)
    sd a5, 128(t0)
    sd a6, 136(t0)
    sd a7, 144(t0)
    sd s2, 152(t0)
    sd s3, 160(t0)
    sd s4, 168(t0)
    sd s5, 176(t0)
    sd s6, 184(t0)
    sd s7, 192(t0)
    sd s8, 200(t0)
    sd s9, 208(t0)
    sd s10, 216(t0)
    sd s11, 224(t0)
    sd t3, 232(t0)
    sd t4, 240(t0)
    sd t5, 248(t0)
    sd t6, 256(t0)

    csrr t1, sscratch
    sd t1, 0(t0)                // interrupted t0
    csrr t1, scause
    sd t1, 16(t0)
    csrr t1, sepc
    sd t1, 24(t0)

    la sp, msg_trap_stack_top
    mv a0, t0
    call msg_s_handler

    /* t0 is caller-saved: the C call may have clobbered it. Reload. */
    la t0, msg_save

    ld t1, 24(t0)              // sepc, unchanged by this handler
    csrw sepc, t1
    ld ra, 32(t0)
    ld sp, 40(t0)
    ld gp, 48(t0)
    ld tp, 56(t0)
    ld t2, 64(t0)
    ld s0, 72(t0)
    ld s1, 80(t0)
    ld a0, 88(t0)
    ld a1, 96(t0)
    ld a2, 104(t0)
    ld a3, 112(t0)
    ld a4, 120(t0)
    ld a5, 128(t0)
    ld a6, 136(t0)
    ld a7, 144(t0)
    ld s2, 152(t0)
    ld s3, 160(t0)
    ld s4, 168(t0)
    ld s5, 176(t0)
    ld s6, 184(t0)
    ld s7, 192(t0)
    ld s8, 200(t0)
    ld s9, 208(t0)
    ld s10, 216(t0)
    ld s11, 224(t0)
    ld t3, 232(t0)
    ld t4, 240(t0)
    ld t5, 248(t0)
    ld t6, 256(t0)

    ld t1, 8(t0)
    csrrw t0, sscratch, t0
    sret

    .global msg_phase1
    .align 2

msg_phase1:
    la t0, smode_landing
    csrw sepc, t0
    csrr t0, sstatus
    ori t0, t0, 0x100          // SPP=1; SIE left as M-mode set it (clear)
    csrw sstatus, t0
    la t0, msg_save
    csrw sscratch, t0
    la t0, 1f
    la t1, msg_resume_pc
    sd t0, 0(t1)
    li t0, 1
    la t1, msg_expect_ecall
    sw t0, 0(t1)
    sret
1:
    ret

    .global msg_phase2
    .align 2

msg_phase2:
    la t0, smode_landing
    csrw sepc, t0
    csrr t0, sstatus
    ori t0, t0, 0x100          // SPP=1; SIE left as M-mode set it (set)
    csrw sstatus, t0
    la t0, msg_save
    csrw sscratch, t0
    la t0, 1f
    la t1, msg_resume_pc
    sd t0, 0(t1)
    li t0, 2
    la t1, msg_expect_ecall
    sw t0, 0(t1)
    sret
1:
    ret

    .global smode_landing
    .global smode_spin_end
    .align 2

smode_landing:
    rdcycle t0
    mv t1, t0
    li t2, 200000
1:
    rdcycle t0
    sub t0, t0, t1
    bltu t0, t2, 1b
smode_spin_end:
    ecall
    j smode_spin_end           // unreachable

    .section .bss
    .align 4
    .global msg_save
msg_save:
    .space 264
    .global msg_trap_stack_top
    .space 8192
msg_trap_stack_top:
