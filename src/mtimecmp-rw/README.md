# mtimecmp-rw

A bare-metal RISC-V program that verifies, on the QEMU `virt` board, that the
CLINT `mtimecmp` register for hart 0 (`0x02004000`) is a plain read/write MMIO
register: three patterns (`0x0`, `0x123456789ABCDEF0`, all-ones) written with
32-bit stores each read back exactly, with all `mie` bits clear so the pending
`mip.MTIP` after the `0x0` write cannot be delivered. Runs entirely in M-mode
on hart 0, installs a trap handler that records and parks on any unexpected
trap, disarms the timer back to all-ones at the end, and self-checks every
claim before printing `RESULT: PASS`. Shares only `src/boot.S` and
`src/uart.c` with the other demos; see `PROOF.md` for the measured evidence.
