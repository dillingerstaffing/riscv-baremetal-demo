# mstatus-sd-summary

A bare-metal RISC-V program that verifies, on the QEMU `virt` board, that
`mstatus.SD` (bit 63) is the read-only summary bit for the FS field: with
FS=Off it reads 0, with FS=Initial (enabled but not dirty) it still reads 0,
a single `fadd.d` on 1.5 and 2.25 moves FS to Dirty (field reads 3) and SD to
1 with f0 holding exactly 3.75 (`0x400e000000000000`), writing FS=Clean drops
SD back to 0, and forcing bit 63 to 1 by software leaves SD reading 0. All
`mstatus` writes are read-modify-write preserving the other bits, except the
deliberate bit-63 probe and the final explicit restore, which is required to
match the boot word bit-for-bit. The run installs a counting M-mode trap
handler as hygiene and requires the trap count to be 0, then self-checks
every claim before printing `RESULT: PASS`. Shares only `src/boot.S` and
`src/uart.c` with the other demos; see `PROOF.md` for the measured evidence.
