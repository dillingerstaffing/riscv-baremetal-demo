    // mprv_trap.S: M-mode trap entry for the mstatus.MPRV load
    // privilege test.
    //
    // What this module proves: mstatus.MPRV (bit 17) makes M-mode
    // explicit data loads and stores use the privilege level in
    // mstatus.MPP instead of the current mode. With MPRV=1 and
    // MPP=U, a load from a page covered by an unlocked PMP TOR entry
    // with R=W=X=0 raises a load access fault (mcause=0x5) with mepc
    // exactly at the faulting load; with MPRV=0 the same load
    // succeeds (M-mode bypasses unlocked PMP entries), and with
    // MPRV=1 and MPP=M it also succeeds (MPP=M keeps M-mode
    // semantics).
    //
    // mprv_m_trap_entry: M-mode vector. The very first instructions
    // stash t0 in mscratch (the faulting context may hold a live t0)
    // and clear MPRV before touching any memory: while MPRV=1 with
    // MPP=U the handler's own stack saves would be privilege-checked
    // as U-mode accesses and fault, so the bit must be off before
    // the first sd. Then it saves every general-purpose register on
    // the current stack, calls the C handler mprv_m_handler, which
    // records mcause/mepc/mtval, advances mepc by 4 past the one
    // expected synchronous probe load (4 bytes under .option
    // norvc), and mret. Any other trap is reported and parks the
    // hart inside the C handler, so it shows up in the log as a
    // FAIL.

    .section .text
    .global mprv_m_trap_entry
    .align 2
mprv_m_trap_entry:
    // Stash t0 in mscratch before touching anything: the faulting
    // context may hold live values in t0 (the probe asm's register
    // choices are the compiler's), and clearing MPRV needs a
    // register for the mask. CSR accesses touch no data memory, so
    // this is safe while MPRV=1/MPP=U.
    csrw mscratch, t0
    li t0, 0x20000                // MPRV bit 17
    csrc mstatus, t0              // MPRV off before any data access
    addi sp, sp, -240
    sd ra, 0(sp)
    sd gp, 8(sp)
    sd tp, 16(sp)
    csrr t0, mscratch
    sd t0, 24(sp)                 // original t0
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

    call mprv_m_handler

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
