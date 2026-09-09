// strap.S: trap entry and exit for the smode module, built twice.
//
// TRAP_SMODE=1: S-mode vector (stvec). Entry swaps sscratch, stamps
//   rdtime/rdcycle, saves x1-x31 plus sepc, calls the C handler on a
//   dedicated trap stack, and exits with sret.
// TRAP_SMODE=0: M-mode baseline (mtvec). Same protocol on the M-mode
//   CSRs, exits with mret.
//
// The contract the privileged ISA gives a trap is identical in both modes:
// the hart writes the faulting PC to *epc, the cause to *cause, moves the
// interrupt-enable bit to the previous-enable slot and clears it, then
// jumps to *tvec. Everything else (trapframe, *scratch swap protocol,
// stack switch) is built on that contract.
//
// Trapframe layout (must match trapframe_t in ssched.h):
//   regs[0..31]    @ 0,8,...,248
//   epc            @ 256
//   mtime_at_entry @ 264
//   cycle_at_entry @ 272

#if TRAP_SMODE
#define CSR_CAUSE  scause
#define CSR_EPC    sepc
#define CSR_TVEC   stvec
#define CSR_SCRATCH sscratch
#define RET_INSN   sret
#else
#define CSR_CAUSE  mcause
#define CSR_EPC    mepc
#define CSR_TVEC   mtvec
#define CSR_SCRATCH mscratch
#define RET_INSN   mret
#endif

    .section .text
    .global strap_entry
    .align 2

    .equ TF_EPC, 256
    .equ TF_MTIME, 264
    .equ TF_CYCLE, 272

strap_entry:
    csrrw t0, CSR_SCRATCH, t0  // t0 = trapframe; scratch = interrupted t0
    rdtime t1
    sd t1, TF_MTIME(t0)
    rdcycle t1
    sd t1, TF_CYCLE(t0)

    // Save x1-x31. x5 (t0) is recovered from scratch further down.
    sd ra, 8(t0)
    sd sp, 16(t0)
    sd gp, 24(t0)
    sd tp, 32(t0)
    sd t1, 48(t0)
    sd t2, 56(t0)
    sd s0, 64(t0)
    sd s1, 72(t0)
    sd a0, 80(t0)
    sd a1, 88(t0)
    sd a2, 96(t0)
    sd a3, 104(t0)
    sd a4, 112(t0)
    sd a5, 120(t0)
    sd a6, 128(t0)
    sd a7, 136(t0)
    sd s2, 144(t0)
    sd s3, 152(t0)
    sd s4, 160(t0)
    sd s5, 168(t0)
    sd s6, 176(t0)
    sd s7, 184(t0)
    sd s8, 192(t0)
    sd s9, 200(t0)
    sd s10, 208(t0)
    sd s11, 216(t0)
    sd t3, 224(t0)
    sd t4, 232(t0)
    sd t5, 240(t0)
    sd t6, 248(t0)
    csrr t1, CSR_SCRATCH
    sd t1, 40(t0)               // regs[5] = interrupted t0
    csrr t1, CSR_EPC
    sd t1, TF_EPC(t0)

    la sp, strap_stack_top     // C handler runs on the dedicated trap stack
    mv a0, t0                  // argument: the trapframe just saved
    call strap_handler         // returns the next trapframe in a0

    // --- exit path: a0 = next trapframe ---
    // Stamp the end of the switch before touching the next context.
    rdcycle t1
    la t2, g_exit_cycles
    sd t1, 0(t2)
    rdtime t1
    la t2, g_exit_mtime
    sd t1, 0(t2)

    mv t1, a0                  // t1 = next trapframe; it is restored last
    ld t2, TF_EPC(t1)
    csrw CSR_EPC, t2
    ld ra, 8(t1)
    ld sp, 16(t1)
    ld gp, 24(t1)
    ld tp, 32(t1)
    // x5 (t0) is restored below; x6 (t1) holds the pointer until the end
    ld t2, 56(t1)
    ld s0, 64(t1)
    ld s1, 72(t1)
    ld a0, 80(t1)
    ld a1, 88(t1)
    ld a2, 96(t1)
    ld a3, 104(t1)
    ld a4, 112(t1)
    ld a5, 120(t1)
    ld a6, 128(t1)
    ld a7, 136(t1)
    ld s2, 144(t1)
    ld s3, 152(t1)
    ld s4, 160(t1)
    ld s5, 168(t1)
    ld s6, 176(t1)
    ld s7, 184(t1)
    ld s8, 192(t1)
    ld s9, 200(t1)
    ld s10, 208(t1)
    ld s11, 216(t1)
    ld t3, 224(t1)
    ld t4, 232(t1)
    ld t5, 240(t1)
    ld t6, 248(t1)
    ld t0, 40(t1)              // restore the next context's t0
    csrw CSR_SCRATCH, t1       // the next trap saves into the next trapframe
    ld t1, 48(t1)              // restore the next context's t1 last
    RET_INSN

    // Dedicated stack for the C trap handler, so handler C code never runs
    // on an interrupted task's stack.
    .section .bss
    .align 4
    .space 16384
strap_stack_top:
