# sie WARL write/legalized-readback probe of STIE (backlog item "riscv sie-stie-write")

Probes the `sie` CSR (supervisor interrupt enable, CSR 0x104) as a WARL
register in M-mode: which enable bits a write admits, how the admitted
set follows `mideleg` delegation, and that `sie.STIE` (bit 5)
round-trips through `csrs`/`csrc` set/clear with the bit reading back
as written. No interrupt source is armed or pended and no trap is
expected; the trap count stays 0 for the whole run.

This is the register-legalization sibling of `src/sie-stie-gate/`,
which tests interrupt-gating behavior (a pended STIP with STIE clear
produces no trap; setting STIE produces exactly one trap). The two
modules test different mechanisms and share no behavior.

## What it does

1. Boots in M-mode, installs a counting park-on-entry trap vector
   (direct-mode `mtvec`). MIE stays clear and no interrupt source is
   armed, so no trap should ever fire.
2. Records the boot `sie` readback (0x0) and the boot `mideleg`
   readback (0x1444, the forced H-extension bits).
3. Delegates only the supervisor timer interrupt (`mideleg` = 0x20;
   readback 0x1464, forced bits plus STI), so STIE is the one S-mode
   enable bit the hart may admit.
4. Writes all-ones to `sie` and publishes the legalized readback:
   0x20. Only the delegated STIE sticks; SSIE and SEIE stay clear
   because SSI and SEI remain M-mode interrupts.
5. Writes zero to `sie`; the readback returns to 0x0.
6. Sets STIE via `csrs sie, t0` (bit 5 is not encodable in the 5-bit
   `csrsi` immediate); the readback is 0x20, the bit set and alone.
7. Clears STIE via `csrc sie, t0`; the readback is 0x0.
8. Restores `sie` to 0 and `mideleg` to 0x1444, readback-verified.
9. Prints the trap count (0), a 64-bit FNV-1a checksum over the
   deterministic measured values, and `RESULT: PASS` only if all 15
   checks held. On PASS it shuts the machine down via the virt
   test-device finisher so the QEMU process exits 0; on FAIL it
   parks the hart instead.
