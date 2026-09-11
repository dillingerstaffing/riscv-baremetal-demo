# scause WARL check

Measures which bits of the `scause` CSR are software-writable
on the hart, and confirms trap entry still overwrites the
register with the real cause. The S-mode counterpart of the
shipped `src/mcause-warl` module (which probed the M-mode
cause register).

## What it does

1. From M-mode, writes all-ones to `scause` with `csrw` and
   publishes the legalized readback; requires the readback to
   equal all-ones (the write took), to be a fixed point of a
   second identical write, and the M-mode trap count to be
   unchanged (no trap fired from the write).
2. Writes zero to `scause`; requires the readback to equal
   zero (no bits are read-only), with the trap count still
   unchanged.
3. Delegates supervisor environment calls (writes exactly
   `medeleg` bit 9; the boot value already delegated illegal
   instructions, and the phase-complete signal below must
   reach M-mode), writes all-ones to `scause` a final time,
   opens the address space with one PMP NAPOT entry, and
   drops to S-mode.
4. Executes `ecall` at a labeled site in S-mode; requires
   exactly one S-mode trap with `scause` = 9, `sepc` exactly
   at the ecall site, `sstatus.SPP` = 1, and the M-mode trap
   count still 0, proving trap entry overwrote the last
   software write with the real cause.
5. Hands back to M-mode with a deliberate illegal
   instruction; requires the M-mode trap count to be 1,
   `scause` to still read 9, and restores `medeleg` to its
   boot value.

Measured finding: on this hart (QEMU 8.2.2, virt machine) all
64 bits of `scause` are software-writable, and a delegated
S-mode trap overwrites the register with the trap cause.

## Files

- `scw_main.c`: the test sequence, UART reporting, PASS/FAIL
  verdict, and the virt test-device finisher shutdown.
- `scw_trap.S`: the M-mode trap entry.
- `scw_strap.S`: the S-mode trap entry.
- `PROOF.md`: the full proof record with per-run measurements.
- `bench-logs/`: the build log and the three raw QEMU run logs.

## Build and run

From the repo root:

```
make scause-warl.elf
make run-scause-warl
```

PASS is reported as `RESULT: PASS` with `checks=23
mismatches=0` and the finisher shuts the machine down, so QEMU
exits 0. FAIL parks the hart in a `wfi` loop without touching
the finisher; under `timeout` that shows up as exit status
124.
