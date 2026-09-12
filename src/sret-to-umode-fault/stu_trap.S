// stu_trap.S: trap entries, phase drivers, and the S/U-mode landing
// pads for the sret-to-umode-fault module.
//
// What this module proves: sret is a privileged instruction,
// executable only in S-mode or M-mode. Phase 1 (the control) runs
// sret in S-mode with SPP=1 and observes it return to S-mode with
// no trap; phase 2 drops to U-mode and runs sret there, and the
// hart must trap to M-mode with mcause=2 (illegal instruction),
// mepc at the sret, and mstatus.MPP=0.
//
// m_trap_entry (M-mode, direct-mode mtvec): mscratch points at the
// 8-word m_regs array. Bumps the M-mode trap counter, records
// mcause, mepc, mtval and mstatus of the trap, using only t0/t1
// (both parked and restored). Two scripted traps exist:
//   * the phase-1 S-mode ecall (mcause 0x9) while g_expect_ecall is
//     set: records ecall_cause/ecall_epc, sets mstatus.MPP=M-mode,
//     points mepc at phase2_drop, clears g_expect_ecall, and mrets.
//   * anything else: restores t0/t1 and jumps to the C
//     continuation m_after_trap (the script ends at the phase-2
//     trap; no mret).
//
// s_trap_entry (S-mode, direct-mode stvec): sscratch points at the
// 2-word s_trap_words array. Bumps the S-mode trap counter and parks
// the hart: no trap may reach S-mode during the run (medeleg is 0).
//
// s_landing (S-mode, entered once via sret from main with SPP=1):
// sets control_ok, records the address of its own sret into
// control_sret_site (in-asm numeric local label, never a C
// computed-goto label), points sepc at the label past the sret,
// keeps SPP=1, and executes sret. The control claim is that this
// sret does NOT trap: the hart must arrive at the fall-through
// label still in S-mode.
//
// s_after (S-mode, the fall-through): sets control_after and issues
// the one scripted ecall back to M-mode (medeleg bit 9 is clear, so
// the ecall traps in M-mode with mcause 0x9). Its address is
// recorded in ecall_site before the ecall.
//
// phase2_drop (M-mode, entered once via mret from the ecall path):
// points sepc at u_landing, clears sstatus.SPP, and srets. sret is
// legal in M-mode, so this drops to U-mode without trapping.
//
// u_landing (U-mode): records the address of the sret below into
// expected_site (in-asm numeric local label), then executes sret.
// U-mode has no right to execute sret, so the hart must trap to
// M-mode with mcause=2. Any fall-through past the sret means the
// instruction did not trap, which contradicts the privilege model,
// so the hart parks.
//
// Slot map (m_regs): 0 M-mode trap count, 1 parked t1, 2 mcause,
// 3 mepc, 4 mtval, 5 mstatus at trap entry.

    .section .text
    .global m_trap_entry
    .align 2
m_trap_entry:
    csrrw t0, mscratch, t0   // t0 = &m_regs, park interrupted t0
    sd t1, 1*8(t0)           // park interrupted t1
    ld t1, 0*8(t0)           // bump the M-mode trap counter
    addi t1, t1, 1
    sd t1, 0*8(t0)
    csrr t1, mcause
    sd t1, 2*8(t0)           // slot 2: mcause of this trap
    csrr t1, mepc
    sd t1, 3*8(t0)           // slot 3: mepc of this trap
    csrr t1, mtval
    sd t1, 4*8(t0)           // slot 4: mtval of this trap
    csrr t1, mstatus
    sd t1, 5*8(t0)           // slot 5: mstatus (MPP = pre-trap mode)

    // Phase-1 return path: the scripted S-mode ecall.
    csrr t1, mcause
    li t2, 9
    bne t1, t2, m_to_cont
    la t1, g_expect_ecall
    lw t1, 0(t1)
    beqz t1, m_to_cont
    // Expected ecall: record it, then redirect to phase2_drop in
    // M-mode.
    csrr t1, mcause
    la t2, ecall_cause
    sd t1, 0(t2)
    csrr t1, mepc
    la t2, ecall_epc
    sd t1, 0(t2)
    csrr t1, mstatus
    li t2, 0x1800
    not t2, t2
    and t1, t1, t2           // clear MPP
    li t2, 0x1800
    or t1, t1, t2            // MPP = M-mode (3)
    csrw mstatus, t1
    la t1, phase2_drop
    csrw mepc, t1
    la t1, g_expect_ecall
    sw zero, 0(t1)
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, mscratch, t0   // restore the interrupted t0
    mret

m_to_cont:
    ld t1, 1*8(t0)           // restore the interrupted t1
    // One swap restores the interrupted t0 and parks &m_regs back
    // in mscratch.
    csrrw t0, mscratch, t0
    // The script ends at the phase-2 trap: jump to the M-mode C
    // continuation, which checks the recorded values and finishes.
    // The interrupted stack pointer is still the boot stack (the
    // U-mode landing pad never touched sp), so the C call is safe.
    j m_after_trap

    .global s_trap_entry
    .align 2
s_trap_entry:
    csrrw t0, sscratch, t0   // t0 = &s_trap_words, park interrupted t0
    sd t1, 1*8(t0)           // park interrupted t1
    ld t1, 0*8(t0)           // bump the S-mode trap counter
    addi t1, t1, 1
    sd t1, 0*8(t0)
    ld t1, 1*8(t0)           // restore the interrupted t1
    csrrw t0, sscratch, t0
s_park:                      // an S-mode trap is off-script: park
    wfi
    j s_park

    .global s_landing
    .align 2
s_landing:
    // Control phase: this pad runs in S-mode (phase 1 dropped here
    // via sret with SPP=1). Flag arrival.
    li t0, 1
    la t1, control_ok
    sd t0, 0(t1)
    // Record the address of the S-mode sret below.
    la t0, 1f
    la t1, control_sret_site
    sd t0, 0(t1)
    // Return address for the sret: the fall-through label.
    la t0, 2f
    csrw sepc, t0
    csrr t0, sstatus
    ori t0, t0, 0x100        // SPP=1: sret must stay in S-mode
    csrw sstatus, t0
1:  sret                     // control: legal in S-mode, no trap
2:
    // Still in S-mode iff the sret above did not trap.
    li t0, 1
    la t1, control_after
    sd t0, 0(t1)
    la t0, 3f
    la t1, ecall_site
    sd t0, 0(t1)
3:  ecall                    // scripted return to M-mode, mcause 0x9
    j s_park                 // unreachable on a correct hart

    .global phase2_drop
    .align 2
phase2_drop:
    // M-mode: drop to U-mode. sret is legal in M-mode, so this
    // sret takes SPP=0 and lands in U-mode without trapping.
    la t0, u_landing
    csrw sepc, t0
    csrr t0, sstatus
    li t1, 0x100
    not t1, t1
    and t0, t0, t1           // SPP=0
    csrw sstatus, t0
    sret
    // Never returns: the U-mode pad traps.

    .global u_landing
    .align 2
u_landing:
    // Record the address of the sret below so the C checker can
    // compare it against the trapped mepc.
    la t1, 1f
    la t2, expected_site
    sd t1, 0(t2)
    .option push
    .option norvc            // sret is a 4-byte instruction
1:  sret                     // U-mode: must trap, mcause 2
    .option pop
    // Unreached on a correct hart: the sret above traps.
    j u_park
u_park:
    wfi
    j u_park
