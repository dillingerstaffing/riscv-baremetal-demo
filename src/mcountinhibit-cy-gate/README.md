# mcountinhibit-cy-gate

A bare-metal RISC-V program that verifies, on the QEMU `virt` board, the exact
gating behavior of the `mcountinhibit.CY` bit for `mcycle`: with CY set, 1000
back-to-back `mcycle` reads all return the identical value (zero advance); with
CY cleared, the counter resumes and bounded-spin samples strictly increase. The
run executes entirely in M-mode on hart 0, installs a trap handler that records
and parks on any unexpected trap, and self-checks every claim before printing
`RESULT: PASS`. Shares only `src/boot.S` and `src/uart.c` with the other demos;
see `PROOF.md` for the measured evidence.
