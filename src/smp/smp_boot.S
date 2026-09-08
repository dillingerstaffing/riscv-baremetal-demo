// smp_boot.S: SMP entry point for the QEMU virt board.
//
// QEMU starts every hart at the ELF entry point in M-mode when booted
// with -bios none -kernel. The first thing each hart does is read its
// hart ID from the mhartid CSR and install a private stack: hart N gets
// the top of hart_stacks[N], a 4 KiB slice of a static array.
//
// Hart 0 then clears BSS and runs the primary bring-up (smp_main).
// Every other hart spins on the smp_release flag (a word in .data, so it
// reads 0 from the loaded image even before hart 0 finishes clearing
// BSS) and, once released, runs the secondary-hart entry (hart1_main).

    .section .text
    .global _start

    .equ HART_STACK_SIZE, 4096

_start:
    // t0 = hart ID from the mhartid CSR.
    csrr t0, mhartid

    // sp = hart_stacks + (hartid + 1) * HART_STACK_SIZE.
    la   t1, hart_stacks
    li   t2, HART_STACK_SIZE
    addi t3, t0, 1
    mul  t3, t3, t2
    add  sp, t1, t3

    // Secondary harts park here; hart 0 continues to BSS clearing.
    bnez t0, hart1_wait

    // Hart 0 only: clear BSS so C globals start at zero.
    la t0, _bss_start
    la t1, _bss_end
bss_loop:
    bgeu t0, t1, bss_done
    sd zero, 0(t0)
    addi t0, t0, 8
    j bss_loop
bss_done:
    call smp_main

halt0:
    wfi
    j halt0

hart1_wait:
    la t1, smp_release
1:
    // fence orders the flag read against hart 0's flag write.
    fence
    lw t0, 0(t1)
    beqz t0, 1b
    // Released: sp is already this hart's private stack.
    call hart1_main

halt1:
    wfi
    j halt1
