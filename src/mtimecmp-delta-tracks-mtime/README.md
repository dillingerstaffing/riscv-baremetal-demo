# mtimecmp-delta-tracks-mtime

A bare-metal RISC-V program that verifies, on the QEMU `virt` board, that the
CLINT `mtime` register is a free-running counter: 12 read pairs separated by a
spin of at least 10000 ticks (1 ms of virtual time) must each show a strictly
positive delta, and 8 immediate back-to-back read pairs must never go backward
(zero deltas are possible when both reads land in the same tick and are
reported, not failed). Reads use the 32-bit stable-pair form (high, low,
high) because 64-bit CLINT accesses fault on this emulator. The run executes
entirely in M-mode on hart 0, installs a trap handler that records and parks
on any unexpected trap, and self-checks every claim (33 checks) before
printing `RESULT: PASS`. Shares only `src/boot.S` and `src/uart.c` with the
other demos; see `PROOF.md` for the measured evidence.
