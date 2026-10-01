# mhartid-s-mode-read

Verifies the privilege gate on the `mhartid` CSR on the QEMU `virt` board: M-mode reads the hart ID as `0x0`, then the hart drops to S-mode and reads the same register, which raises an illegal-instruction trap (scause `0x2`) delivered to S-mode through medeleg bit 2, with the destination register never overwritten. A control read of `sstatus` in S-mode succeeds with no new trap. See [PROOF.md](PROOF.md) for the measured results.
