<!-- PROOF-HEADER
Checks: 7
Mismatches: 0
Checksum: 0x5f6b8f12ec83304a
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mcountinhibit.IR gates minstret (freeze under IR, resume on clear)

## What was built

`src/mcountinhibit-ir-freeze/`: a bare-metal RISC-V program that verifies, on
the QEMU `virt` board, the exact gating behavior of the `mcountinhibit.IR`
bit (bit 2) for `minstret`. With IR set, 1000 back-to-back `csrr minstret`
reads must all return the identical value (zero advance); with IR cleared
again, the counter must resume, and samples taken in bounded spins must
strictly increase. The whole experiment runs in M-mode on hart 0 (QEMU
boots the ELF straight into M-mode with `-bios none`, so no privilege
drop is needed), and the module shares only `src/boot.S` and `src/uart.c`
with the other demos.

- `mirf_main.c`: records `minstret` and the boot `mcountinhibit` value,
  writes `mcountinhibit = 0x4` (IR) and requires the readback to carry
  the bit, then runs the freeze phase (1000 `minstret` reads into a BSS
  array, every readback compared against the first); clears IR, requires
  the exact-zero readback, then runs the resume phase (4 samples, each
  re-read in a bounded 2^20 spin until it exceeds the previous, because
  back-to-back reads can land in the same host tick); finally prints and
  requires a zero trap count. A 64-bit FNV-1a checksum is fed the
  measured samples in a fixed order (the 1000 freeze samples, then the
  resume samples) and printed on the completion path; because the
  absolute counter values differ per run, the checksum is run-specific.
- `mirf_trap.S`: minimal M-mode trap entry (direct mode). Any trap during
  this experiment is unexpected (the program never issues a trapping
  instruction), so the handler records `mcause`/`mepc`/`mtval` and a trap
  count into `mirf_save`, then parks the hart in a `wfi` loop. Reaching
  the printed verdict already implies zero traps, and the program also
  checks the count explicitly.
- `PROOF.md` (this file), `bench-logs/` with the build log, three raw
  QEMU run logs, and `host-cpu.txt`.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS` is
printed only when all 7 checks held. On PASS the machine is shut down
through the virt test-device finisher (QEMU exits 0); on FAIL the hart
parks in a `wfi` loop without touching the finisher, so a FAIL is
observable as exit status 124 under `timeout`.

Build: `make mcountinhibit-ir-freeze.elf` with the repo Makefile
(`-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie
-fno-pie -fno-pic -march=rv64imac_zicsr`), logged in
`bench-logs/build.log`. (`boot.o` links first so `_start` lands at
0x80000000, the address QEMU's `-kernel` loader starts at.)
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mcountinhibit-ir-freeze.elf` under `timeout` so a parked-hart FAIL
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
except for the absolute `minstret` values (a live counter) and their
derived deltas and checksums; every verdict-relevant line is identical
across runs.

| step | operation | measured readback |
|---|---|---|
| boot | `csrr mcountinhibit`, `csrr minstret` | mcountinhibit=0x0 on all 3 runs; minstret live (0x4616dee3c98, 0x46172136357, 0x461787170a9) |
| gate | `csrw mcountinhibit, 0x4` (IR); readback | 0x4 on all 3 runs |
| freeze | 1000 back-to-back `minstret` reads with IR=1 | every readback 0x0 on all 3 runs; freeze delta (last - first) = 0; samples differing from the first = 0 |
| unblock | `csrw mcountinhibit, 0x0`; readback | 0x0 on all 3 runs |
| resume | 4 `minstret` samples with IR=0, bounded spin per sample | strictly increasing on all 3 runs; consecutive deltas positive (run1: 19950, 72450, 750; run2: 19410, 98430, 825; run3: 26310, 134940, 735); total deltas 93150, 118665, 161985 |
| traps | handler record | 0 on all 3 runs |

checks: 7, mismatches: 0, `RESULT: PASS` on all 3 runs. Run checksums:
run1 0x5f6b8f12ec83304a (also stamped in the header above), run2
0x3de397993d13ee2a, run3 0x6768c06cd9b34f75; they differ because the
live counter values differ.

Note: the frozen readback (0x0 on all runs) is QEMU-specific and is
reported, not asserted. The asserted mechanism is zero advance while
IR is inhibited (every readback identical), which held on all 3 runs.
