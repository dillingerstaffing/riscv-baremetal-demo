// rpc_trap.S: M-mode trap entry for the WFI resume-PC module.
//
// Exactly one trap source is ever enabled (the CLINT machine software
// interrupt), and the trap is always taken with mepc at the wfi
// instruction under test. The handler:
//   1. saves x1-x31 (t0 via the mscratch swap),
//   2. records mcause and mepc for the C-side checks,
//   3. clears the msip bit with a 32-bit store (the source is
//      level-triggered: it must read 0 before mret restores MIE, or
//      the trap would re-fire immediately),
//   4. advances the saved mepc by 4, past the wfi,
//   5. restores every register bit-identically and executes mret.
//
// There is deliberately no C call: any compiler-generated prologue or
// epilogue would touch the very registers the module compares, so the
// whole handler is hand-written assembly. mscratch points at rpc_save;
// its layout is x1..x31 at (regno*8), mcause at 256, mepc at 264.

    .section .text
    .global rpc_trap_entry
    .align 2
rpc_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &rpc_save; interrupted t0 -> mscratch
    sd x1, 1*8(t0)
    sd x2, 2*8(t0)
    sd x3, 3*8(t0)
    sd x4, 4*8(t0)
    // x5 (t0) is parked in mscratch for now
    sd x6, 6*8(t0)
    sd x7, 7*8(t0)
    sd x8, 8*8(t0)
    sd x9, 9*8(t0)
    sd x10, 10*8(t0)
    sd x11, 11*8(t0)
    sd x12, 12*8(t0)
    sd x13, 13*8(t0)
    sd x14, 14*8(t0)
    sd x15, 15*8(t0)
    sd x16, 16*8(t0)
    sd x17, 17*8(t0)
    sd x18, 18*8(t0)
    sd x19, 19*8(t0)
    sd x20, 20*8(t0)
    sd x21, 21*8(t0)
    sd x22, 22*8(t0)
    sd x23, 23*8(t0)
    sd x24, 24*8(t0)
    sd x25, 25*8(t0)
    sd x26, 26*8(t0)
    sd x27, 27*8(t0)
    sd x28, 28*8(t0)
    sd x29, 29*8(t0)
    sd x30, 30*8(t0)
    sd x31, 31*8(t0)
    csrr t1, mscratch        // the interrupted t0
    sd t1, 5*8(t0)
    csrr t1, mcause
    sd t1, 32*8(t0)
    csrr t1, mepc
    sd t1, 33*8(t0)

    // Publish what the trap saw, for the C-side checks.
    la t1, rpc_mcause
    ld t2, 32*8(t0)
    sd t2, 0(t1)
    la t1, rpc_mepc
    ld t2, 33*8(t0)
    sd t2, 0(t1)

    // Clear the msip bit for hart 0. QEMU 8.2.2's CLINT model only
    // implements 4-byte accesses to the msip registers (a 64-bit
    // access faults; verified while building the msip module), so
    // this is a 32-bit store to 0x02000000.
    lui t1, 0x2000           // 0x2000 << 12 = 0x02000000
    sw zero, 0(t1)

    // Advance the saved mepc past the wfi instruction.
    ld t2, 33*8(t0)
    addi t2, t2, 4
    sd t2, 33*8(t0)

    // Count the trap.
    la t1, rpc_traps
    ld t2, 0(t1)
    addi t2, t2, 1
    sd t2, 0(t1)

    // Restore: mepc first (t1 still scratch), then every register.
    // t0 (x5) is restored last via the mscratch swap, which also
    // re-parks &rpc_save in mscratch for the next trap.
    ld t1, 33*8(t0)
    csrw mepc, t1
    ld x1, 1*8(t0)
    ld x2, 2*8(t0)
    ld x3, 3*8(t0)
    ld x4, 4*8(t0)
    ld x6, 6*8(t0)
    ld x7, 7*8(t0)
    ld x8, 8*8(t0)
    ld x9, 9*8(t0)
    ld x10, 10*8(t0)
    ld x11, 11*8(t0)
    ld x12, 12*8(t0)
    ld x13, 13*8(t0)
    ld x14, 14*8(t0)
    ld x15, 15*8(t0)
    ld x16, 16*8(t0)
    ld x17, 17*8(t0)
    ld x18, 18*8(t0)
    ld x19, 19*8(t0)
    ld x20, 20*8(t0)
    ld x21, 21*8(t0)
    ld x22, 22*8(t0)
    ld x23, 23*8(t0)
    ld x24, 24*8(t0)
    ld x25, 25*8(t0)
    ld x26, 26*8(t0)
    ld x27, 27*8(t0)
    ld x28, 28*8(t0)
    ld x29, 29*8(t0)
    ld x30, 30*8(t0)
    ld x31, 31*8(t0)
    csrrw t0, mscratch, t0
    mret

    // Trap save area: 34 words (x1..x31 at regno*8, mcause at 256,
    // mepc at 264). Zeroed by boot.S's BSS clearing.
    .section .bss
    .align 4
    .global rpc_save
rpc_save:
    .space 34*8
