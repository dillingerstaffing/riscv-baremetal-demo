<!-- PROOF-HEADER
Checks: 15
Mismatches: 0
Checksum: 0x4d74f3e95211308e
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mcounteren.CY gates the S-mode cycle counter, open and shut

## What was built

`src/mcounteren-cy-gate/`: a bare-metal RISC-V program that
verifies, on the QEMU `virt` board, that `mcounteren.CY` (bit 0)
gates S-mode reads of the cycle counter in both directions: with
`mcounteren = 0x0` an S-mode `rdcycle` raises an
illegal-instruction exception instead of returning a count, and
with `mcounteren = 0x1` the same read succeeds and the counter
advances. This is the cycle-counter counterpart of
`src/mcounteren-ir-gate` (instret) and
`src/mcounteren-time-gate` (time); the three experiments differ
only in which counter bit is gated. M-mode reads are never
gated, so the experiment requires a real privilege drop to
observe the gate. The module shares only `src/boot.S` and
`src/uart.c` with the other demos.

- `mcg_main.c`: M-mode setup (boot `mcounteren` readback, 0x7
  writability probe, gate write of 0x0, `mtvec`/`stvec` install,
  `mscratch` pointed at the M-mode record, `sscratch` at the
  S-mode record, `medeleg` bit 2 so the illegal-instruction trap
  is delivered to S-mode while the phase-B ecall stays in
  M-mode, a whole-address-space PMP NAPOT entry, then `mret`
  with MPP=01 into the S-mode payload). The S-mode payload runs
  phase A (labeled `rdcycle` with `mcounteren.CY = 0`,
  expecting exactly one S-mode trap with `scause = 2` and `sepc`
  exactly at the rdcycle site, the destination register
  untouched), phase B (a labeled `ecall` back to M-mode, where
  the handler sets `mcounteren = 0x1` and records the readback;
  QEMU resets `mcounteren` to 0, so the set is explicit, and the
  verdict asserts `mcause = 9`, `mepc` exactly at the ecall site,
  and a readback of 0x1), and phase C (two labeled `rdcycle`
  samples with `mcounteren.CY = 1`, expecting no new trap, a
  strictly positive first sample, and a strictly increasing
  second sample). The fault sites use in-asm numeric local
  labels (`la t, 1f` with `1:` in the asm) so the assembler
  resolves the exact address; the C `&&label` construct is never
  used for trap-resume addresses. A 64-bit FNV-1a checksum is fed
  the fifteen verdict-relevant values in a fixed order from both
  privilege levels and printed on the completion path. Absolute
  cycle-counter samples are deliberately excluded from the
  checksum: they are timing values that differ run to run.
- `mcg_strap.S`: S-mode trap entry (direct mode). Records
  `scause`/`stval`/`sepc`, bumps the trap counter, advances
  `sepc` by 4 (exact: `rdcycle` has no compressed encoding), and
  `sret`.
- `mcg_mtrap.S`: M-mode trap entry (direct mode). Handles the
  one expected ecall from S-mode (records `mcause`/`mepc`/count,
  sets `mcounteren = 0x1`, records the readback, advances `mepc`
  by 4, `mret` back to S-mode with MPP still 01); any other
  M-mode trap parks the hart.
- `build.sh`, `PROOF.md` (this file), `bench-logs/` with the build
  log and three raw QEMU run logs.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 15 checks held. On PASS the machine is
shut down through the virt test-device finisher (QEMU exits 0); on
FAIL the hart parks in a `wfi` loop without touching the finisher.

Build: direct `riscv64-unknown-elf-gcc` invocations matching the
repo Makefile pattern (`-Wall -Wextra -O2 -ffreestanding
-nostdlib -nostartfiles -no-pie -fno-pie -fno-pic
-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`), logged in
`bench-logs/build.log` via `build.sh`; the Makefile itself was not
modified.
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mcounteren-cy-gate.elf` under `timeout`, so a parked-hart
FAIL is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the experiment drops to S-mode via
  `mret` with `mstatus.MPP = 01`, then takes one M-mode excursion
  via `ecall` between the phases.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`),
  `riscv64-unknown-elf-gcc` 13.2.0, `-march=rv64imac_zicsr`.
- `medeleg = 0x4` (only the illegal-instruction trap delegated to
  S-mode; the S-mode ecall is deliberately kept in M-mode).

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs exited 0 via the test-device finisher. The three logs are
byte-identical except for the absolute `rdcycle` sample values
(which are a live cycle counter); every verdict-relevant line is
identical across runs.

| step | operation | measured readback |
|---|---|---|
| boot | `csrr mcounteren` | 0x0 on all 3 runs |
| probe | `csrw mcounteren, 0x7`; readback | 0x7 on all 3 runs |
| gate | `csrw mcounteren, 0x0`; readback | 0x0 (CY clear) on all 3 runs |
| setup | `medeleg` readback | 0x4 on all 3 runs |
| phase A | S-mode `rdcycle` at 0x80000288 with `mcounteren = 0x0` | trap, `scause = 0x2`, `stval = 0xc0002973`, `sepc = 0x80000288` (exactly the rdcycle site), `sepc+4 = 0x8000028c` (the resume label), trap count 1, dest register still the 0xdeadbeefdeadbeef sentinel |
| phase B | S-mode `ecall` at 0x800003d2, handled in M-mode | `mcause = 0x9`, `mepc = 0x800003d2` (exactly the ecall site), M-mode trap count 1, `mcounteren` readback after set 0x1 (CY set, TM and IR clear) |
| phase C | S-mode `rdcycle` with CY set | no new trap (S-mode trap count still 1), samples strictly positive and strictly increasing: deltas +7515, +7485, +7080 across the three runs |

Checks: 15 (probe readback, gate readback, medeleg bit 2,
phase-A trap count == 1, scause == 2, sepc == fault site,
sepc+4 == resume label, sentinel intact, phase-B M-mode trap
count == 1, mcause == 9, mepc == ecall site, mcounteren readback
== 0x1, phase-C S-mode trap count still 1, first sample strictly
positive, samples advancing). Mismatches: 0.
FNV-1a checksum over the fifteen verdict-relevant values:
0x4d74f3e95211308e, identical on all 3 runs.

Note: `stval = 0xc0002973` is QEMU's informational readback of
the faulting `rdcycle` encoding for the illegal-instruction trap;
it is printed as observed and is not part of the verdict or the
checksum.

Note on the phase-C deltas: in this QEMU build (no `-icount`) the
`cycle` counter is derived from the host clock, so two adjacent
`rdcycle` instructions report a few thousand ticks of elapsed wall
time rather than an instruction count. The verdict asserts only
strict positivity and strict increase, which is exactly what the
mechanism under test (the CY gate being open) guarantees; the
absolute values are timing noise and differ across runs.

## Limits of verification

- The +4 `sepc` advance in the S-mode handler is exact only
  because `rdcycle` has no compressed encoding; the module
  asserts the result (`sepc+4` equals the labeled resume address)
  rather than assuming it.
- The gate is exercised for the CY bit only; TM gating was
  covered by `src/mcounteren-time-gate` and IR gating by
  `src/mcounteren-ir-gate`.
- QEMU-specific readbacks observed but not asserted: boot
  `mcounteren = 0x0` and the `stval` encoding above.
- No restore of `mcounteren` is performed: the machine halts on
  the completion path, and M-mode restore semantics are already
  covered by `src/mcounteren`.
- U-mode gating was not tested; the module drops M-mode to S-mode
  only, with one ecall excursion back to M-mode.
- Emulator behavior, not silicon: every number above was measured
  under QEMU 8.2.2 `virt`, not on a physical hart.
