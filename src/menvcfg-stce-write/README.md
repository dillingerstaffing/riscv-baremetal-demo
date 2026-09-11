# menvcfg-stce-write

A bare-metal RISC-V program that verifies, on the QEMU `virt` board, the
write/readback behavior of the `menvcfg.STCE` bit (bit 63) in M-mode: the bit
reads back exactly as written (set: write boot|STCE, readback equals it; clear:
write boot&~STCE, readback equals it), with no other bit disturbed by either
write. An all-ones WARL probe reports the legalized readback honestly (STCE
survives, boot-set bits do not vanish, two all-ones writes legalize
identically); the machine is restored to the boot `menvcfg` value before the
verdict. The run installs a trap handler that records and parks on any
unexpected trap and requires the trap count to be 0, then self-checks every
claim before printing `RESULT: PASS`. Shares only `src/boot.S` and
`src/uart.c` with the other demos; see `PROOF.md` for the measured evidence.
