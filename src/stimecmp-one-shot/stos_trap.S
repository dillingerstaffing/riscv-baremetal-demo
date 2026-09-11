    // stos_trap.S: trap entries for the stimecmp one-shot module.
    //
    // stos_trap_entry: S-mode vector. Entry swaps t0 with sscratch so t0
    // points at stos_save, parks the interrupted t1, then saves every
    // general-purpose register before calling the C handler on a dedicated
    // trap stack. The handler takes the one expected supervisor timer
    // trap, records scause/sepc, disarms the timer by writing all-ones to
    // stimecmp, and counts any further trap (there must be none). Exit
    // restores every register, reinstalls stos_save in sscratch, and sret
    // resumes the interrupted code with sepc unchanged.
    //
    // m_trap_entry: M-mode vector. No M-mode trap is expected after boot:
    // entry swaps t0 with mscratch, records mcause/mepc, and calls the C
    // function m_unexpected_trap, which prints the trap and parks the
    // hart. An M-mode trap therefore shows up in the log as a FAIL, not
    // as a silent hang.

    .section .text
    .global stos_trap_entry
    .align 2

stos_trap_entry:
    csrrw t0, sscratch, t0
    sd t1, 8(t0)

    sd ra, 48(t0)
    sd sp, 56(t0)
    sd gp, 64(t0)
    sd tp, 72(t0)
    sd t2, 80(t0)
    sd s0, 88(t0)
    sd s1, 96(t0)
    sd a0, 104(t0)
    sd a1, 112(t0)
    sd a2, 120(t0)
    sd a3, 128(t0)
    sd a4, 136(t0)
    sd a5, 144(t0)
    sd a6, 152(t0)
    sd a7, 160(t0)
    sd s2, 168(t0)
    sd s3, 176(t0)
    sd s4, 184(t0)
    sd s5, 192(t0)
    sd s6, 200(t0)
    sd s7, 208(t0)
    sd s8, 216(t0)
    sd s9, 224(t0)
    sd s10, 232(t0)
    sd s11, 240(t0)
    sd t3, 248(t0)
    sd t4, 256(t0)
    sd t5, 264(t0)
    sd t6, 272(t0)

    csrr t1, sscratch
    sd t1, 0(t0)                // interrupted t0
    csrr t1, scause
    sd t1, 16(t0)
    csrr t1, sepc
    sd t1, 24(t0)

    la sp, stos_trap_stack_top
    mv a0, t0
    call stos_trap_handler

    /* t0 is caller-saved: the C call may have clobbered it. Reload. */
    la t0, stos_save

    ld t1, 24(t0)              // sepc, unchanged by this handler
    csrw sepc, t1
    ld ra, 48(t0)
    ld sp, 56(t0)
    ld gp, 64(t0)
    ld tp, 72(t0)
    ld t2, 80(t0)
    ld s0, 88(t0)
    ld s1, 96(t0)
    ld a0, 104(t0)
    ld a1, 112(t0)
    ld a2, 120(t0)
    ld a3, 128(t0)
    ld a4, 136(t0)
    ld a5, 144(t0)
    ld a6, 152(t0)
    ld a7, 160(t0)
    ld s2, 168(t0)
    ld s3, 176(t0)
    ld s4, 184(t0)
    ld s5, 192(t0)
    ld s6, 200(t0)
    ld s7, 208(t0)
    ld s8, 216(t0)
    ld s9, 224(t0)
    ld s10, 232(t0)
    ld s11, 240(t0)
    ld t3, 248(t0)
    ld t4, 256(t0)
    ld t5, 264(t0)
    ld t6, 272(t0)
    la t1, stos_save
    csrw sscratch, t1          // reinstall the save area for the next trap
    ld t1, 8(t0)               // interrupted t1
    ld t0, 0(t0)               // interrupted t0
    sret

    // Dedicated stack for the C trap handler.
    .section .bss
    .align 4
    .space 8192
stos_trap_stack_top:

    // M-mode unexpected-trap vector: record mcause/mepc and park.
    .section .text
    .global m_trap_entry
    .align 2

m_trap_entry:
    csrrw t0, mscratch, t0
    sd t1, 0(t0)
    csrr t1, mcause
    sd t1, 8(t0)
    csrr t1, mepc
    sd t1, 16(t0)
    la sp, m_trap_stack_top
    call m_unexpected_trap
m_park:
    wfi
    j m_park

    .section .bss
    .align 4
    .space 4096
m_trap_stack_top:
