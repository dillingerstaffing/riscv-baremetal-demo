<!-- PROOF-HEADER
Checks: 33
Mismatches: 0
Checksum: 0x3499a18272e6aff0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: CLINT mtime advance/delta measurement

## What was built

`src/mtimecmp-delta-tracks-mtime/`: a bare-metal RISC-V program that verifies,
on the QEMU `virt` board, that the CLINT `mtime` register is a free-running
counter. Two files, sharing only `src/boot.S` and `src/uart.c` with the other
demos. Exactly one mechanism is under test: whether repeated reads of `mtime`
observe a counter that never goes backward and advances across a real delay.

- `mdt_main.c`: reads the CLINT `mtime` at `0x0200bff8` with 32-bit
  stable-pair loads only (high, low, high; the pair is kept only when the
  two high reads agree, so a 32-bit boundary crossing cannot corrupt the
  64-bit value). 64-bit CLINT accesses are never issued: they fault on this
  emulator (observed with the msip register in `src/msip/`). The program
  runs 12 delayed pairs (read t0, spin until `mtime` has advanced at least
  10000 ticks, read t1) requiring a strictly positive delta, then 8
  immediate back-to-back pairs (read t0, read t1) requiring the second
  read to be no smaller than the first. The spin is bounded by a 2^28
  iteration guard, so a frozen counter would trip the guard and fail
  instead of hanging. The back-to-back check compares the raw values
  (`t1 >= t0`); it deliberately does not go through the unsigned delta,
  which would wrap on a backward step and hide it. Finally the program
  prints the CLINT readback addresses, the trap record, and requires a
  zero trap count. A 64-bit FNV-1a checksum is fed every t0/t1 in a fixed
  order and printed on the completion path; because `mtime` is a live
  counter, the checksum is run-specific.
- `mdt_trap.S`: minimal M-mode trap entry (direct mode). Any trap during
  this experiment is unexpected (the program only issues plain MMIO loads
  with interrupts disabled: `mie` and `mstatus.MIE` are 0 at reset), so
  the handler records `mcause`/`mepc`/`mtval` and a trap count into
  `mdt_save`, then parks the hart in a `wfi` loop. Reaching the printed
  verdict already implies zero traps, and the program also checks the
  count explicitly.
- `PROOF.md` (this file), `bench-logs/` with the build log and three raw
  QEMU run logs.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS` is
printed only when all 33 checks held. On PASS the machine is shut down
through the virt test-device finisher (QEMU exits 0); on FAIL the hart
parks in a `wfi` loop without touching the finisher, so a FAIL is
observable as exit status 124 under `timeout`.

Build: `make mtimecmp-delta-tracks-mtime.elf` with the repo Makefile
(`-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie
-fno-pie -fno-pic -march=rv64imac_zicsr`), logged in
`bench-logs/build.log`. (`boot.o` links first so `_start` lands at
0x80000000, the address QEMU's `-kernel` loader starts at.)
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mtimecmp-delta-tracks-mtime.elf` under `timeout` so a parked-hart FAIL
is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart, M-mode throughout. QEMU boots the ELF
  straight into M-mode with `-bios none`.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`),
  `riscv64-unknown-elf-gcc` 13.2.0 (Debian), `-march=rv64imac_zicsr`.
- `mtvec` points at the park-on-trap handler; no delegation (M-mode
  keeps every trap). No interrupt enable bits are touched.

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three runs
exited 0 via the test-device finisher. The three logs are structurally
identical; only the absolute `mtime` values (a live counter), the
derived deltas, and the checksums differ. No `FAIL` line appears in any
log (verified: zero occurrences of the string "FAIL" across all three).

Delayed pairs (spin of at least 10000 ticks between the reads; every
delta strictly positive on all 3 runs):

| run | the 12 deltas (ticks) | min | max | mean |
|---|---|---|---|---|
| 1 | 10585, 10027, 10020, 10020, 10017, 10024, 22522, 10021, 10024, 47914, 16198, 10020 | 10017 | 47914 | 14782 |
| 2 | 10440, 10021, 10025, 10027, 13576, 11769, 10019, 10025, 10024, 10021, 10025, 10019 | 10019 | 13576 | 10499 |
| 3 | 11627, 17529, 10679, 10031, 10018, 10020, 10023, 10021, 10022, 10020, 10026, 10162 | 10018 | 17529 | 10848 |

The deltas cluster just above the 10000-tick spin (the extra ~20 ticks
are the two stable-pair reads themselves); the occasional larger delta
(host scheduling jitter under the emulator) is reported, not hidden.
The smallest delayed delta observed across all 36 pairs is 10017 ticks.

Back-to-back pairs (immediate re-read; the counter never went backward
on any of the 24 pairs):

| run | the 8 deltas (ticks) | min | max | zero-deltas |
|---|---|---|---|---|
| 1 | 1570, 19, 19, 14, 20, 15, 13, 14 | 13 | 1570 | 0/8 |
| 2 | 641, 19, 17, 19, 20, 16, 19, 19 | 16 | 641 | 0/8 |
| 3 | 856, 18, 14, 13, 13, 13, 15, 15 | 13 | 856 | 0/8 |

A steady-state back-to-back delta of 13 to 20 ticks is the cost of two
stable-pair reads (six 32-bit loads plus loop overhead) at the 10 MHz
timebase; the first pair of each run is larger because it follows the
delayed-phase print loop. No pair in any run showed a zero delta (both
reads landing in the same tick did not occur at this read latency) and
no pair showed a negative step.

Readback addresses printed by the program: `lo=0x0200bff8`,
`hi=0x0200bffc`, the CLINT `mtime` register pair. Trap record on all 3
runs: `count=0 mcause=0x0 mepc=0x0 mtval=0x0`.

checks: 33, mismatches: 0, `RESULT: PASS` on all 3 runs. Run checksums:
run1 0x3499a18272e6aff0 (also stamped in the header above), run2
0xd8362d9dc494e7e7, run3 0xadfbae0f0de87871; they differ because the
live counter values differ.

## What was not verified

Only the CLINT MMIO path at `0x0200bff8` was exercised. The `time` CSR
was not read, so no claim is made about CSR/MMIO agreement. The
timebase rate (10 MHz nominal) was not measured against a wall clock;
only monotonicity and the strictly positive delayed deltas were
verified. Single hart only.
