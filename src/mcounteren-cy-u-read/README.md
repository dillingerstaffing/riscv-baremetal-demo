# mcounteren-cy-u-read

A bare-metal RISC-V program that verifies, on the QEMU `virt` board, the exact
gating behavior of the `mcounteren.CY` bit for U-mode: with CY set a U-mode
`rdcycle` succeeds and returns strictly increasing samples, and with CY clear
the same U-mode `rdcycle` raises an illegal-instruction exception
(`scause = 2`, destination register untouched). The S-level gate
(`scounteren.CY`) is held set for the whole run so it can never mask the
M-level gate under test; this module is the converse of
`src/scounteren-cy-gate`, which held the M-level gate set and toggled the
S-level one. The run drops M-mode to S-mode to U-mode twice (once per phase),
delegates the illegal-instruction trap and the U-mode `ecall` to S-mode, hands
back to M-mode between the phases with an S-mode `ecall` (cause 9, not
delegated) that verifies the handoff and clears `mcounteren.CY`, and
self-checks every claim before printing `RESULT: PASS`. Shares only
`src/boot.S` and `src/uart.c` with the other demos; see `PROOF.md` for the
measured evidence.
