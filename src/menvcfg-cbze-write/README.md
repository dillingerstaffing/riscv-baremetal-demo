# menvcfg-cbze-write

A bare-metal RISC-V program that verifies, on the QEMU `virt` board, the
write/readback behavior of the `menvcfg.CBZE` field (bits 7:6) in M-mode: each
legal value 0, 1, 2, 3 reads back exactly as written (write
(boot&~0xC0)|(v<<6), readback equals it, no other bit disturbed). An all-ones
WARL probe reports the legalized readback honestly (CBZE field survives,
boot-set bits do not vanish, two all-ones writes legalize identically); the
machine is restored to the exact boot `menvcfg` value before the verdict. The
run installs a trap handler that records and parks on any unexpected trap and
requires the trap count to be 0, then self-checks every claim before printing
`RESULT: PASS`. Shares only `src/boot.S` and `src/uart.c` with the other
demos; see `PROOF.md` for the measured evidence.
