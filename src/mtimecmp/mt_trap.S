// mt_trap.S: M-mode trap entry for the mtimecmp accuracy module.
//
// The first instructions are the measurement: the earliest observable
// points of the trap are stamped before anything else is touched.
//   1: csrrw swaps t0 with mscratch, so t0 points at mt_save
//   2: the interrupted t1 is parked (it is clobbered by the stamps)
//   3: rdtime -> mtime_entry (tick domain, the primary clock here)
//   5: rdcycle -> cycle_entry (secondary fine-grained view)
// Then x1-x31 are saved, mcause/mepc recorded, and the C handler runs
// on a dedicated trap stack. The handler disarms the timer (mtimecmp =
// all-ones) and sets mt_flag, so mret returns into the trial's spin
// loop, which sees the flag and exits. Exit restores every register,
// reinstalls mt_save in mscratch, and mret resumes the trial.

    .section .text
    .global mt_trap_entry
    .align 2

mt_trap_entry:
    csrrw t0, mscratch, t0
    sd t1, 8(t0)
    rdtime t1
    sd t1, 32(t0)
    rdcycle t1
    sd t1, 40(t0)

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

    csrr t1, mscratch
    sd t1, 0(t0)                // interrupted t0
    csrr t1, mcause
    sd t1, 16(t0)
    csrr t1, mepc
    sd t1, 24(t0)

    la sp, mt_trap_stack_top
    mv a0, t0
    call mt_trap_handler

    /* t0 is caller-saved: the C call may have clobbered it. Reload. */
    la t0, mt_save

    ld t1, 24(t0)              // mepc, unchanged by this handler
    csrw mepc, t1
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
    la t1, mt_save
    csrw mscratch, t1          // reinstall the save area for the next trap
    ld t1, 8(t0)               // interrupted t1
    ld t0, 0(t0)               // interrupted t0
    mret

    // Dedicated stack for the C trap handler.
    .section .bss
    .align 4
    .space 8192
mt_trap_stack_top:
