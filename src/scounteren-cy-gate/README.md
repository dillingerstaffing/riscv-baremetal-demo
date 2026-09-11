# scounteren-cy-gate

A bare-metal RISC-V program that verifies, on the QEMU `virt` board, the exact
gating behavior of the `scounteren.CY` bit for U-mode: with CY clear a U-mode
`rdcycle` raises an illegal-instruction exception (`scause = 2`, destination
register untouched) instead of returning the cycle count, and with CY set the
same U-mode `rdcycle` succeeds and returns strictly increasing samples. The
run drops M-mode to S-mode to U-mode twice (once per phase), delegates the
illegal-instruction trap and the U-mode `ecall` to S-mode, and self-checks
every claim before printing `RESULT: PASS`. Shares only `src/boot.S` and
`src/uart.c` with the other demos; see `PROOF.md` for the measured evidence.
