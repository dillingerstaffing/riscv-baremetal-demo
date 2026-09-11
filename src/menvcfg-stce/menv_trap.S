    // menv_trap.S: trap entries for the menvcfg-STCE probe module.
    //
    // menv_strap_entry: S-mode vector. Swaps t0 with sscratch so t0 points
    // at menv_save, parks the interrupted t1, saves every general-purpose
    // register, then records scause and the original sepc. If the trap is
    // a synchronous exception (scause bit 63 clear) sepc is advanced by 4
    // before the C handler runs: the only exception this module can
    // produce on purpose is the phase-1 stimecmp access probe, a 4-byte
    // CSR instruction emitted under .option norvc, so +4 resumes exactly
    // at the labeled resume address. Interrupts keep sepc unchanged.
    // The C handler then runs on a dedicated trap stack; exit restores
    // every register, reinstalls menv_save in sscratch, and sret resumes.
    //
    // menv_mtrap_entry: M-mode vector. No M-mode trap is expected after
    // boot: entry swaps t0 with mscratch, records mcause/mepc, and calls
    // the C function menv_m_unexpected_trap, which prints the trap and
    // parks the hart. An M-mode trap therefore shows up in the log as a
    // FAIL, not as a silent hang.

    .section .text
    .global menv_strap_entry
    .align 2

menv_strap_entry:
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
    sd t1, 24(t0)               // original sepc, before any adjust

    // Exception (bit 63 clear): advance sepc by 4 so the probe resumes
    // past the faulting 4-byte CSR instruction. Interrupt (bit 63 set):
    // leave sepc alone.
    csrr t1, scause
    bltz t1, 1f
    csrr t1, sepc
    addi t1, t1, 4
    csrw sepc, t1
1:
    csrr t1, sepc
    sd t1, 264(t0)             // sepc as adjusted: exceptions get +4,
                              // interrupts keep the original value
    la sp, menv_trap_stack_top
    mv a0, t0
    call menv_s_trap_handler

    /* t0 is caller-saved: the C call may have clobbered it. Reload. */
    la t0, menv_save

    ld t1, 264(t0)             // the adjusted sepc, computed at entry
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
    la t1, menv_save
    csrw sscratch, t1          // reinstall the save area for the next trap
    ld t1, 8(t0)               // interrupted t1
    ld t0, 0(t0)               // interrupted t0
    sret

    // Dedicated stack for the C trap handler.
    .section .bss
    .align 4
    .space 8192
menv_trap_stack_top:

    // M-mode unexpected-trap vector: record mcause/mepc and park.
    .section .text
    .global menv_mtrap_entry
    .align 2

menv_mtrap_entry:
    csrrw t0, mscratch, t0
    sd t1, 0(t0)
    csrr t1, mcause
    sd t1, 8(t0)
    csrr t1, mepc
    sd t1, 16(t0)
    la sp, menv_mtrap_stack_top
    call menv_m_unexpected_trap
menv_mpark:
    wfi
    j menv_mpark

    .section .bss
    .align 4
    .space 4096
menv_mtrap_stack_top:
