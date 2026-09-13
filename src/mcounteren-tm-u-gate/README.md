# mcounteren-tm-u-gate

A bare-metal RISC-V program that verifies, on the QEMU `virt` board, the exact
gating behavior of the `mcounteren.TM` bit for U-mode: with TM set a U-mode
`rdtime` succeeds and returns strictly increasing samples, with TM clear the
same U-mode `rdtime` raises an illegal-instruction exception (`scause = 2`,
`sepc` exactly at the rdtime site, destination register untouched), and with
TM restored the read succeeds again. `scounteren.TM` is held set for the whole
run so the M-level gate is the only one under test. The run drops M-mode to
S-mode to U-mode three times (control, gate, restore), delegates the
illegal-instruction trap and the U-mode `ecall` to S-mode while keeping the
S-mode `ecall` as an M-mode phase handoff, then restores `mcounteren`,
`scounteren`, and `medeleg` to their boot values before shutting down with
`RESULT: PASS`. Shares only `src/boot.S` and `src/uart.c` with the other
demos; see `PROOF.md` for the measured evidence.
