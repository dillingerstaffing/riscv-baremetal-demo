// boot.S: machine entry point for the QEMU virt board.
// QEMU loads this ELF at 0x80000000 and jumps to _start in M-mode.

    .section .text
    .global _start

_start:
    // Set up a stack for C code. _stack_top is defined in link.ld.
    la sp, _stack_top

    // Clear the BSS section so globals start at zero.
    la t0, _bss_start
    la t1, _bss_end
bss_loop:
    bgeu t0, t1, bss_done
    sd zero, 0(t0)
    addi t0, t0, 8
    j bss_loop
bss_done:

    call main

    // main returned: park the hart.
halt:
    wfi
    j halt
