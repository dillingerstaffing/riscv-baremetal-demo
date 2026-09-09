// mtv_trap.S: vectored-mode M-mode trap entry for the mtvec vectored
// dispatch module (backlog item 74).
//
// The vector table is 16 consecutive entries of exactly one 4-byte
// instruction each. Compressed encodings are disabled inside the table
// (.option norvc) so every entry is a 32-bit jal: entry N sits at
// BASE + 4*N and executes `jal x0, mtv_stub_N`.
//
// Each stub records the link-time address of its own table entry
// (via la, resolved exactly by the assembler) and its cause index,
// parks the interrupted t1 in its save slot, then falls into the
// common handler. The common handler saves mcause, mepc, an entry
// cycle stamp, and every caller-saved register the C call may
// clobber, calls mtv_c_handle(), then restores everything, honors a
// possible return-to-M-mode request from the C handler, writes back
// the resume mepc, and mret.
//
// mscratch points at the mtv_regs array in mtv_main.c. The stub swaps
// t0 through mscratch, so the interrupted t0 is preserved across the
// whole trap. Across the C call, &mtv_regs is parked in mscratch
// (t0 is caller-saved and cannot be trusted after the call).
//
// The U-mode entry helper below drops to U-mode with mret (MPP=0)
// into a two-ecall payload: the first ecall resumes in U-mode at the
// second ecall, the second returns to M-mode at mtv_m_resume.

    .section .text
    .align 8
    .global mtv_table
mtv_table:
.Lvec_base:

    .option push
    .option norvc
    .macro vec_entry n
    .global mtv_vec\n
mtv_vec\n:
    jal x0, mtv_stub_\n
    .endm
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    vec_entry \n
    .endr
    .option pop

    // Table layout (16 entries x 4 bytes) is verified at run time:
    // mtv_main.c checks entry7 == BASE+28 and entry8 == BASE+32.

    .align 2
    .macro stub_def n
    .global mtv_stub_\n
mtv_stub_\n:
    csrrw t0, mscratch, t0   // t0 = &mtv_regs, interrupted t0 parked
    sd t1, 6*8(t0)           // park interrupted t1 in its save slot
    la t1, mtv_vec\n         // exact entry address, assembler-resolved
    sd t1, 0*8(t0)           // landing address observed by this trap
    li t1, \n
    sd t1, 1*8(t0)           // cause index of the stub that ran
    j mtv_common
    .endm
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    stub_def \n
    .endr

    .align 2
    .global mtv_common
mtv_common:
    // On entry: t0 = &mtv_regs, interrupted t0 parked in mscratch,
    // interrupted t1 already saved at slot 6 by the stub.
    csrr t1, mcause
    sd t1, 2*8(t0)
    csrr t1, mepc
    sd t1, 3*8(t0)
    csrr t1, cycle
    sd t1, 4*8(t0)
    sd ra, 5*8(t0)
    sd t2, 7*8(t0)
    sd t3, 8*8(t0)
    sd t4, 9*8(t0)
    sd t5, 10*8(t0)
    sd t6, 11*8(t0)
    sd a0, 12*8(t0)
    sd a1, 13*8(t0)
    sd a2, 14*8(t0)
    sd a3, 15*8(t0)
    sd a4, 16*8(t0)
    sd a5, 17*8(t0)
    sd a6, 18*8(t0)
    sd a7, 19*8(t0)
    // Park &mtv_regs in mscratch across the C call: t0 is
    // caller-saved and unusable after it.
    csrrw t0, mscratch, t0
    call mtv_c_handle
    csrrw t0, mscratch, t0   // t0 = &mtv_regs again
    // If the C handler asked to return to M-mode (second U-mode
    // ecall), switch MPP to M and resume at mtv_m_resume.
    la t1, mtv_return_m
    ld t1, 0(t1)
    beqz t1, 1f
    li t1, (3 << 11)
    csrs mstatus, t1         // MPP = 3 (M-mode)
    la t1, mtv_m_resume
    sd t1, 20*8(t0)
1:
    ld ra, 5*8(t0)
    ld t2, 7*8(t0)
    ld t3, 8*8(t0)
    ld t4, 9*8(t0)
    ld t5, 10*8(t0)
    ld t6, 11*8(t0)
    ld a0, 12*8(t0)
    ld a1, 13*8(t0)
    ld a2, 14*8(t0)
    ld a3, 15*8(t0)
    ld a4, 16*8(t0)
    ld a5, 17*8(t0)
    ld a6, 18*8(t0)
    ld a7, 19*8(t0)
    ld t1, 20*8(t0)      // resume mepc chosen by the C handler
    csrw mepc, t1
    ld t1, 6*8(t0)       // restore the interrupted t1
    csrrw t0, mscratch, t0 // restore the interrupted t0, park &mtv_regs
    mret

    // U-mode entry: mret with MPP=0 into the two-ecall payload.
    // Control returns to the caller via mtv_m_resume after the second
    // ecall's trap handler switches MPP back to M.
    .align 2
    .global mtv_umode_entry
mtv_umode_entry:
    la t0, mtv_umode_payload
    csrw mepc, t0
    li t0, (3 << 11)
    csrc mstatus, t0      // MPP = 0 (U-mode)
    li t0, (1 << 7)
    csrs mstatus, t0      // MPIE = 1, so the later mret back to M-mode
                          // restores MIE=1 (without this the drop's mret
                          // clears MIE and interrupts stay wedged)
    mret

    .align 2
mtv_umode_payload:
    ecall                 // trap 1: code 8, resumes at the next ecall
    ecall                 // trap 2: code 8, handler returns to M-mode

    .align 2
    .global mtv_m_resume
mtv_m_resume:
    ret
