# mideleg WARL write/legalized-readback check

Publishes the interrupt causes QEMU 8.2.2's virt hart will let
M-mode delegate, as declared by the WARL legalized readback of
`mideleg`.

## What it does

1. Reads `mideleg` at boot: it reads 0x1444, the implementation's
   forced-on bits.
2. Installs a defensive park-on-entry trap handler (direct-mode
   `mtvec`); no interrupt source is armed and MIE stays clear, so
   any trap is a FAIL observable as a harness timeout.
3. Writes all-ones to `mideleg` with `csrw` and publishes the
   write/legalized-readback pair; requires the readback to equal
   0x3666 (the delegable interrupt causes).
4. Writes zero to `mideleg`; requires the readback to equal
   0x1444, not 0: bits 2, 6, 10, 12 are read-only-one on this hart
   and cannot be cleared by software.
5. Writes all-ones again; requires the readback to repeat 0x3666,
   proving the legalization is stable across writes.

Measured finding: on this hart an all-ones write legalizes to
0x3666 (delegable causes 1, 2, 5, 6, 9, 10, 12, 13) and the zero
write reads back the forced bits 0x1444. The values match QEMU
8.2.2's `rmw_mideleg64` write path (target/riscv/csr.c), which
masks writes to 0x2666 and forces on 0x1444.

## Files

- `mideleg_main.c`: the test sequence, UART reporting, PASS/FAIL
  verdict, and the virt test-device finisher shutdown.
- `mideleg_trap.S`: the defensive park-on-entry trap handler.
- `PROOF.md`: the full proof record with per-run measurements.
- `bench-logs/`: the build log and the three raw QEMU run logs.

## Build and run

From the repo root:

```
make mideleg-warl.elf
make run-mideleg-warl
```

PASS is reported as `RESULT: PASS (checks=6)` and the finisher
shuts the machine down, so QEMU exits 0. FAIL parks the hart in a
`wfi` loop without touching the finisher; under `timeout` that
shows up as exit status 124.
