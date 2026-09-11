# mstatus-mprv-readback

A bare-metal RISC-V program that verifies, on the QEMU `virt` board, the
write/readback behavior of the `mstatus.MPRV` bit (bit 17) in M-mode: the bit
reads back set after `csrs` with no other bit disturbed (readback equals
boot|MPRV exactly), and reads back clear after `csrc` with the register equal
to the boot value exactly. The machine is confirmed back at the boot `mstatus`
value after the probes. The run installs a trap handler that records and parks
on any unexpected trap and requires the trap count to be 0, then self-checks
every claim before printing `RESULT: PASS`. A design note: while MPRV is set
(with boot MPP=U and no PMP entries), a data load or store is privilege-checked
as U-mode and faults, so the set/probe/clear sequence runs inside one
CSR-only asm block with no memory access between the `csrs` and the `csrc`.
Shares only `src/boot.S` and `src/uart.c` with the other demos; see `PROOF.md`
for the measured evidence.
