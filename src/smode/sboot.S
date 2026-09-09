// sboot.S: machine entry for the smode module (both binaries).
// QEMU loads the ELF at 0x80000000 and jumps to _start in M-mode.
//
// _start runs in M-mode: it clears BSS, sets up the M-mode stack, and calls
// m_boot() (C, per-binary behavior). For the S-mode binary, m_boot sets up
// trap delegation and executes sret into s_entry, never returning. For the
// M-mode baseline binary, m_boot is a stub and _start falls through to
// main, still in M-mode.
//
// s_entry is the S-mode landing point after the sret: it switches to the
// S-mode boot stack and calls main. sscratch already points at the boot
// trapframe (set by m_boot), so the first timer trap saves main's context
// there.

    .section .text
    .global _start
    .global s_entry

_start:
    la sp, _stack_top

    la t0, _bss_start
    la t1, _bss_end
bss_loop:
    bgeu t0, t1, bss_done
    sd zero, 0(t0)
    addi t0, t0, 8
    j bss_loop
bss_done:

    call m_boot
    call main

halt:
    wfi
    j halt

// S-mode entry after m_boot's sret. Runs with the S-mode stack.
s_entry:
    la sp, _s_stack_top
    call main

s_halt:
    wfi
    j s_halt

// m_trap_entry: minimal M-mode trap handler for the S-mode binary.
// Only used when the hart lacks Sstc: the S-mode scheduler issues an ecall
// (service 1, deadline in a1) to have M-mode reprogram mtimecmp, since
// mtimecmp is M-mode-only. Anything else parks the hart.
//
// The handler borrows the M-mode ecall stack via mscratch (the S-mode sp
// is parked in mscratch on entry and restored before mret; the swap is
// balanced because MIE is clear inside the handler, so no nesting).

    .global m_trap_entry
    .align 2
m_trap_entry:
    csrrw sp, mscratch, sp
    addi sp, sp, -32
    sd t0, 0(sp)
    sd a0, 8(sp)
    sd a1, 16(sp)
    csrr t0, mcause
    li a0, 9                    // supervisor environment call
    bne t0, a0, m_trap_halt
    ld a0, 8(sp)                // service number
    li t0, 1                    // service 1: rearm timer, deadline in a1
    bne a0, t0, m_trap_halt
    ld a1, 16(sp)
    li t0, 0x02004000          // CLINT mtimecmp, hart 0
    sd a1, 0(t0)
    csrr t0, mepc
    addi t0, t0, 4              // resume past the ecall
    csrw mepc, t0
    ld t0, 0(sp)
    ld a0, 8(sp)
    ld a1, 16(sp)
    addi sp, sp, 32
    csrrw sp, mscratch, sp
    mret
m_trap_halt:
    wfi
    j m_trap_halt

    // S-mode boot stack (16 KiB). M-mode keeps using _stack_top from
    // link.ld; the M-mode ecall handler gets its own 4 KiB stack below so
    // it never touches S-mode's saved stack region. Each label marks the
    // TOP of its stack (stacks grow down).
    .section .bss
    .align 4
    .space 16384
    .global _s_stack_top
_s_stack_top:
    .align 4
    .space 4096
    .global m_estack_top
m_estack_top:
