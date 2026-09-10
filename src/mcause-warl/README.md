# mcause WARL check (backlog item 148)

Measures which bits of the `mcause` CSR are software-writable on
the hart, and confirms trap entry still overwrites the register.

## What it does

1. Installs a minimal M-mode trap handler (direct-mode `mtvec`,
   `mscratch` scratch area) that records `mcause`/`mepc`, bumps a
   trap counter, and resumes past the 4-byte `ecall`.
2. Issues an M-mode `ecall` and requires the handler-recorded cause
   and an independent `csrr` readback to both report 11.
3. Writes all-ones to `mcause` with `csrw` and publishes the
   write/last-cause/readback triple; requires the readback to equal
   all-ones (the write took) and the trap count to be unchanged.
4. Writes zero to `mcause`; requires the readback to equal zero.
5. Issues a second `ecall`; requires trap count 2 and the readback
   `mcause` = 11, proving trap entry overwrites the register
   regardless of the last software write.

Measured finding: on this hart (QEMU 8.2.2, virt machine) all 64
bits of `mcause` are software-writable. Writes are not ignored.

## Files

- `mcause_main.c`: the test sequence, UART reporting, PASS/FAIL
  verdict, and the virt test-device finisher shutdown.
- `mcause_trap.S`: the trap entry.
- `PROOF.md`: the full proof record with per-run measurements.
- `bench-logs/`: the build log and the three raw QEMU run logs.

## Build and run

From the repo root:

```
make mcause-warl.elf
make run-mcause-warl
```

PASS is reported as `RESULT: PASS (traps=2)` and the finisher
shuts the machine down, so QEMU exits 0. FAIL parks the hart in a
`wfi` loop without touching the finisher; under `timeout` that
shows up as exit status 124.
