// stvv_trap.S: S-mode vectored trap entry for the stvec vectored-offset
// module (backlog item 133).
//
// The vector table is 16 consecutive entries of exactly one 4-byte
// instruction each. Compressed encodings are disabled inside the table
// (.option norvc) so every entry is a 32-bit jal: entry N sits at
// BASE + 4*N and executes `jal x0, stvv_stub_N`.
//
// Each stub swaps t0 through sscratch, records the link-time address
// of its own table entry (via la, resolved exactly by the assembler)
// and its entry index, then falls into the common handler. The common
// handler records scause, sepc, landing address and entry index into
// the stvv_ev_* event arrays in stvv_main.c, then dispatches:
//
//   scause 0x8000000000000005 (supervisor timer): disarms stimecmp by
//     writing ~0 (Sstc CSR 0x14d) and raises stvv_timer_done.
//   scause 0x8000000000000009 (supervisor external): claims the PLIC
//     hart-0 S-mode context (context 1), reads the looped-back UART
//     byte (drops the level-triggered IRQ line), completes the claim,
//     and raises stvv_ext_done.
//   anything else: bumps stvv_unexpected.
//
// Only t0, t1, t2, t3 are touched; all are saved in the stvv_save area
// (sscratch points at it) and restored before sret. No C calls happen
// in the trap path.
//
// m_trap_entry is the M-mode handler used during this module's M-mode
// setup window. No M-mode trap is expected; if one fires, the handler
// records mcause and parks the hart so the run cannot claim success.
//
// stvv_save slot map: 0 unused, 1 parked t1, 2 landing address,
// 3 entry index, 5 sepc, 6 parked t2, 7 parked t3.

    .section .text

// ---- S-mode vectored table ----
    .align 8
    .global stvv_table
stvv_table:

    .option push
    .option norvc
    .macro vec_entry n
    .global stvv_vec\n
stvv_vec\n:
    jal x0, stvv_stub_\n
    .endm
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    vec_entry \n
    .endr
    .option pop

// ---- per-entry stubs ----
    .align 2
    .macro stub_def n
    .global stvv_stub_\n
stvv_stub_\n:
    csrrw t0, sscratch, t0   // t0 = &stvv_save, interrupted t0 in sscratch
    sd t1, 1*8(t0)           // park interrupted t1 in slot 1
    la t1, stvv_vec\n        // exact entry address, assembler-resolved
    sd t1, 2*8(t0)           // slot 2: landing address of this trap
    li t1, \n
    sd t1, 3*8(t0)           // slot 3: entry index of this trap
    j stvv_common
    .endm
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    stub_def \n
    .endr

// ---- common S-mode handler ----
    .align 2
    .global stvv_common
stvv_common:
    // On entry: t0 = &stvv_save, interrupted t0 parked in sscratch,
    // interrupted t1 saved at slot 1, landing address at slot 2,
    // entry index at slot 3.
    sd t2, 6*8(t0)           // park t2, t3 in slots 6, 7
    sd t3, 7*8(t0)
    csrr t2, scause
    csrr t3, sepc
    sd t3, 5*8(t0)           // slot 5: sepc of this trap

    // Append to the event log: scause, landing address, entry index
    // at position stvv_ev_count; park on overflow (cannot happen in
    // this run's two-trap sequence).
    la t3, stvv_ev_count
    ld t1, 0(t3)
    li t3, 4
    bgeu t1, t3, stvv_overflow
    slli t3, t1, 3           // byte offset = count * 8
    la t1, stvv_ev_scause
    add t1, t1, t3
    sd t2, 0(t1)
    ld t1, 2*8(t0)           // landing address from the stub
    la t2, stvv_ev_land
    add t2, t2, t3
    sd t1, 0(t2)
    ld t1, 3*8(t0)           // entry index from the stub
    la t2, stvv_ev_idx
    add t2, t2, t3
    sd t1, 0(t2)
    la t3, stvv_ev_count
    ld t1, 0(t3)
    addi t1, t1, 1
    sd t1, 0(t3)

    // Dispatch on scause.
    csrr t2, scause
    li t1, 0x8000000000000005
    beq t2, t1, stvv_timer_isr
    li t1, 0x8000000000000009
    beq t2, t1, stvv_ext_isr
    la t3, stvv_unexpected
    ld t1, 0(t3)
    addi t1, t1, 1
    sd t1, 0(t3)
    j stvv_resume

stvv_timer_isr:
    li t1, -1
    csrw 0x14d, t1           // stimecmp = ~0: disarm the timer source
    la t3, stvv_timer_done
    li t1, 1
    sd t1, 0(t3)
    j stvv_resume

stvv_ext_isr:
    li t1, 0x0c201004        // PLIC claim, hart 0 S-mode context
    lw t1, 0(t1)
    la t3, stvv_claimed
    sd t1, 0(t3)             // claimed interrupt id
    li t1, 0x10000000
    lbu t1, 0(t1)            // read the looped-back byte; drops the
                             // UART's level-triggered IRQ line
    li t1, 0x0c201004
    li t3, 10
    sw t3, 0(t1)             // complete the UART claim
    la t3, stvv_ext_done
    li t1, 1
    sd t1, 0(t3)
    j stvv_resume

stvv_overflow:
    // More than 4 traps: something is badly wrong; park.
1:  wfi
    j 1b

stvv_resume:
    ld t3, 7*8(t0)           // restore the interrupted t3, t2, t1, t0
    ld t2, 6*8(t0)
    ld t1, 1*8(t0)
    csrrw t0, sscratch, t0
    sret

// ---- M-mode handler (setup window; no trap expected) ----
    .align 2
    .global m_trap_entry
m_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &m_regs, interrupted t0 in mscratch
    sd t1, 1*8(t0)           // park interrupted t1 in slot 1
    csrr t1, mcause
    sd t1, 2*8(t0)           // slot 2: mcause of the unexpected trap
    ld t1, 0*8(t0)           // bump the trap counter in slot 0
    addi t1, t1, 1
    sd t1, 0*8(t0)
    ld t1, 1*8(t0)           // restore t1, t0, then park: an M-mode
    csrrw t0, mscratch, t0   // trap means the run cannot claim success
1:  wfi
    j 1b
