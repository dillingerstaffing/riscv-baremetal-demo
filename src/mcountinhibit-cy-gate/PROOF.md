<!-- PROOF-HEADER
Checks: 7
Mismatches: 0
Checksum: 0x14cd37e90f3ea9db
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mcountinhibit.CY gates mcycle (freeze under CY, resume on clear)

## What was built

`src/mcountinhibit-cy-gate/`: a bare-metal RISC-V program that verifies, on
the QEMU `virt` board, the exact gating behavior of the `mcountinhibit.CY`
bit (bit 0) for `mcycle`. With CY set, 1000 back-to-back `csrr mcycle`
reads must all return the identical value (zero advance); with CY cleared
again, the counter must resume, and samples taken in bounded spins must
strictly increase. The whole experiment runs in M-mode on hart 0 (QEMU
boots the ELF straight into M-mode with `-bios none`, so no privilege
drop is needed), and the module shares only `src/boot.S` and `src/uart.c`
with the other demos.

- `mcy_main.c`: records `mcycle` and the boot `mcountinhibit` value,
  writes `mcountinhibit = 0x1` (CY) and requires the readback to carry
  the bit, then runs the freeze phase (1000 `mcycle` reads into a BSS
  array, every readback compared against the first); clears CY, requires
  the exact-zero readback, then runs the resume phase (4 samples, each
  re-read in a bounded 2^20 spin until it exceeds the previous, because
  back-to-back reads can land in the same host tick); finally prints and
  requires a zero trap count. A 64-bit FNV-1a checksum is fed the
  measured samples in a fixed order (the 1000 freeze samples, then the
  resume samples) and printed on the completion path; because the
  absolute counter values differ per run, the checksum is run-specific.
- `mcy_trap.S`: minimal M-mode trap entry (direct mode). Any trap during
  this experiment is unexpected (the program never issues a trapping
  instruction), so the handler records `mcause`/`mepc`/`mtval` and a trap
  count into `mcy_save`, then parks the hart in a `wfi` loop. Reaching
  the printed verdict already implies zero traps, and the program also
  checks the count explicitly.
- `PROOF.md` (this file), `bench-logs/` with the build log, three raw
  QEMU run logs, and `host-cpu.txt`.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS` is
printed only when all 7 checks held. On PASS the machine is shut down
through the virt test-device finisher (QEMU exits 0); on FAIL the hart
parks in a `wfi` loop without touching the finisher, so a FAIL is
observable as exit status 124 under `timeout`.

Build: `make mcountinhibit-cy-gate.elf` with the repo Makefile
(`-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie
-fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`),
logged in `bench-logs/build.log`. (`boot.o` links first so `_start`
lands at 0x80000000, the address QEMU's `-kernel` loader starts at.)
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mcountinhibit-cy-gate.elf` under `timeout` so a parked-hart FAIL
is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart, M-mode throughout. QEMU boots the ELF
  straight into M-mode with `-bios none`.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`),
  `riscv64-unknown-elf-gcc` 13.2.0 (Debian), `-march=rv64imac_zicsr`.
- `mtvec` points at the park-on-trap handler; no delegation (M-mode
  keeps every trap).

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three runs
exited 0 via the test-device finisher. The three logs are byte-identical
except for the absolute `mcycle` values (a live counter) and their
derived deltas and checksums; every verdict-relevant line is identical
across runs.

| step | operation | measured readback |
|---|---|---|
| boot | `csrr mcountinhibit`, `csrr mcycle` | mcountinhibit=0x0 on all 3 runs; mcycle live (0x2515a5943a6, 0x251621b1010, 0x2516c022ab9) |
| gate | `csrw mcountinhibit, 0x1` (CY); readback | 0x1 on all 3 runs |
| freeze | 1000 back-to-back `mcycle` reads with CY=1 | every readback 0x0 on all 3 runs; freeze delta (last - first) = 0; samples differing from the first = 0 |
| unblock | `csrw mcountinhibit, 0x0`; readback | 0x0 on all 3 runs |
| resume | 4 `mcycle` samples with CY=0, bounded spin per sample | strictly increasing on all 3 runs; consecutive deltas (32445, 109680, 780) / (23820, 77025, 780) / (20025, 73305, 810); total deltas 142905 / 101625 / 94140 |
| traps | handler trap count | 0 on all 3 runs |

Checks: 7 (mcountinhibit CY write readback carries CY, freeze phase
1000 samples all identical to the first, mcountinhibit clear readback
exactly 0, resume bounded spin never tripped, resume samples strictly
increasing, resume total delta positive, trap count 0). Mismatches: 0.
FNV-1a checksum over the measured samples: 0x14cd37e90f3ea9db (run 1),
0xb94a5748f45a2cd7 (run 2), 0x80bbb50de1020194 (run 3). The header above
carries run 1's checksum; the per-run values differ only because
`mcycle` is a live counter.

## Limits of verification

- QEMU-specific readbacks observed but not asserted: while CY is
  inhibited, `mcycle` reads return 0x0 on QEMU 8.2.2 (the counter does
  not latch the pre-gate value); the experiment asserts only the
  guaranteed property, zero advance, i.e. every readback identical.
  After clearing, reads resume the live host-derived counter
  (resume samples continue in the 0x2515..0x2516c range, the same
  range as the pre-gate read).
- Each resume sample is re-read in a bounded spin (2^20 iterations)
  until it exceeds the previous sample, because back-to-back reads
  can land in the same host tick; the bound never tripped on any run,
  and a trip would have been counted as a failed check.
- The gate is exercised for `mcycle` (CY) only; `mtime`/`minstret`
  and the other `mcountinhibit` bits were not tested here.
- Single hart, M-mode only; S-mode/U-mode visibility of the gate was
  not tested here.
- No restore of `mcountinhibit` is performed: the machine halts on the
  completion path.
