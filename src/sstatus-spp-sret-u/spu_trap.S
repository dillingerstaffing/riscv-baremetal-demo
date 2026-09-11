// spu_trap.S: trap entries and the U-mode landing pad for the
// sstatus-spp-sret-u module.
//
// m_trap_entry (M-mode, direct-mode mtvec): mscratch points at the
// 8-word m_regs array. Bumps the M-mode trap counter, records mcause,
// mepc, mtval and mstatus of the trap, restores t0/t1, and jumps to
// the C continuation m_after_trap (the script ends at the trap; no
// mret). mstatus.MPP at entry is the pre-trap privilege mode, which
// the checker requires to be 0 (U-mode).
//
// s_trap_entry (S-mode, direct-mode stvec): sscratch points at the
// 2-word s_trap_words array. Bumps the S-mode trap counter and parks
// the hart: no trap may reach S-mode during the run (medeleg is 0).
//
// u_landing (U-mode, entered once via sret from main): records the
// address of the privileged read below into the C global
// expected_site, then executes it. U-mode has no right to read
// sstatus, so the hart must trap to M-mode with mcause=2. The read
// address is taken with an in-asm numeric local label (1f), never a
// C computed-goto label, so it is exact at any optimization level.
// Any fall-through past the read means the read did not trap, which
// contradicts the privilege model, so the hart parks.
//
// Slot map (m_regs): 0 M-mode trap count, 1 parked t1, 2 mcause,
// 3 mepc, 4 mtval, 5 mstatus at trap entry.
//
// Only t0/t1 are used inside the M-mode handler, both parked and
// restored before the jump to the C continuation.

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
    ld t1, 1*8(t0)           // restore the interrupted t1
    // One swap restores the interrupted t0 and parks &m_regs back
    // in mscratch.
    csrrw t0, mscratch, t0
    // The script ends at this trap: jump to the M-mode C
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

    .global u_landing
    .align 2
u_landing:
    // Record the address of the privileged read so the C checker
    // can compare it against the trapped mepc.
    la t1, 1f
    la t2, expected_site
    sd t1, 0(t2)
1:  csrr t0, sstatus   // privileged read from U-mode: must trap, mcause 2
    // Unreached on a correct hart: the read above traps.
    j u_park
u_park:
    wfi
    j u_park
