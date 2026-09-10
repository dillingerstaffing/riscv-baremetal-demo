<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: M-mode rdcycle is not gated by mcounteren

## What was built

`src/mcounteren/`: a bare-metal RISC-V program that verifies, on the
QEMU `virt` board, that M-mode reads of the `cycle` counter are not
gated by `mcounteren`. Exactly one mechanism is under test: the
`mcounteren` enable bits (CY, TM, IR) gate counter reads in S and U
mode only; a hart executing in M-mode may read the counters
regardless of the `mcounteren` bits. The module shares only
`src/boot.S` and `src/uart.c` with the other demos.

- `mce_main.c`: records the boot `mcounteren` value, establishes a
  baseline that `rdcycle` advances, then (a) probes writability by
  writing `0x7` (CY|TM|IR) to `mcounteren` and requiring the
  readback to be exactly `0x7`, (b) writes `mcounteren = 0`,
  requires the readback to be `0x0`, takes five `rdcycle` samples
  and requires each to be strictly larger than the previous one,
  and (c) restores `mcounteren` to the boot value and requires the
  readback to equal the boot value. A failed check prints `FAIL`
  and flips the verdict; `RESULT: PASS` is printed only when every
  check held. On PASS the machine is shut down through the virt
  test-device finisher (QEMU exits 0); on FAIL the hart parks in a
  `wfi` loop without touching the finisher.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make mcounteren.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mcounteren.elf` (or `make run-mcounteren`), under
`timeout` so a parked-hart FAIL is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- QEMU 8.2.2 `virt` machine, xpack `riscv64-unknown-elf-gcc`
  15.2.0, `-march=rv64imac_zicsr`.
- No trap handler: everything is a plain M-mode CSR read/write;
  writing `0` to `mcounteren` cannot trap a M-mode hart, so there
  is no faulting state to recover from.

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs printed the same `mcounteren` values and `RESULT: PASS`, and
QEMU exited 0 via the test-device finisher on every run (counter
absolute values differ per run, as expected of a live cycle
counter).

| step | operation | measured readback |
|---|---|---|
| boot | `csrr mcounteren` | 0x0 on all 3 runs |
| boot | two `rdcycle` reads (baseline advancement) | deltas +6510, +6645, +6555 |
| probe | `csrw mcounteren, 0x7` | readback 0x7 on all 3 runs |
| gate | `csrw mcounteren, 0` | readback 0x0 on all 3 runs |
| gate | five `rdcycle` samples with `mcounteren = 0` | strictly increasing on all 3 runs; sample deltas run1: +19380, +360, +180, +150; run2: +23775, +435, +165, +165; run3: +20550, +375, +165, +165 |
| restore | `csrw mcounteren, 0x0` (boot value) | readback 0x0 on all 3 runs |

The writability probe is what makes the `0` write meaningful:
QEMU boots with `mcounteren = 0x0`, so a lone `0` write would be
indistinguishable from an ignored write. Because the probe write of
`0x7` read back exactly as written, the subsequent `0` readback is
known to be the written value, not a no-op baseline.

## What was verified, and what was not

Verified: on QEMU 8.2.2, `mcounteren` is writable by M-mode (write
`0x7` reads back `0x7`); with `mcounteren = 0`, M-mode `rdcycle`
reads keep returning strictly advancing values (positive deltas on
every consecutive sample, on all three runs), matching the
specification's rule that the enable bits gate only lower-privilege
counter reads; the CSR was restored to its boot value at the end.

Not verified: reads from S or U mode with `mcounteren = 0` were not
attempted here; whether those reads trap or return zero is outside
this module's verification surface. Nothing was measured on real
silicon; these are QEMU's counter emulation results, so the claim
is "QEMU behaves as the specification requires for M-mode reads",
not a statement about any particular chip.
