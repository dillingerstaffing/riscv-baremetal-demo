# menvcfg-pbmte-write

A bare-metal RISC-V program that verifies, on the QEMU `virt` board, the
write/readback behavior of the `menvcfg.PBMTE` bit (bit 62) in M-mode: a WARL
bit, so the legalized value is read back after every write (the set probe
records whether bit 62 stuck and no other bit may change; the clear probe must
read back exactly; an all-ones WARL probe reports the legalized readback
honestly, asserting only stable legalization and no vanished boot-set bits,
plus PBMTE survival when the set probe proved it implemented); the machine is
restored to the boot `menvcfg` value before the verdict. The run installs a
trap handler that records and parks on any unexpected trap and requires the
trap count to be 0, then self-checks every claim before printing `RESULT:
PASS`. Shares only `src/boot.S` and `src/uart.c` with the other demos; see
`PROOF.md` for the measured evidence.
