/* link.ld: place the kernel at the start of RAM on the QEMU virt board. */
OUTPUT_ARCH(riscv)
ENTRY(_start)

BASE_ADDRESS = 0x80000000;

SECTIONS
{
    . = BASE_ADDRESS;

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    }

    /* 16 KiB stack for the boot hart, 16-byte aligned. */
    . = ALIGN(16);
    _stack_top = . + 0x4000;
}
