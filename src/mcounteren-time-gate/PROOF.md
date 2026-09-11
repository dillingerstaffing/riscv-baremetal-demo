<!-- PROOF-HEADER
Checks: 11
Mismatches: 0
Checksum: 0xd1d0b4647fd56387
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mcounteren gates the S-mode time counter independently of the cycle counter

## What was built

`src/mcounteren-time-gate/`: a bare-metal RISC-V program that
verifies, on the QEMU `virt` board, that `mcounteren` gates each
counter independently for S-mode: with `mcounteren = 0x1` (CY set,
TM clear) an S-mode `rdtime` raises an illegal-instruction
exception instead of returning a count, while an S-mode `rdcycle`
at the same privilege level still reads and advances. This is the
time-counter counterpart of `src/scounteren-trap` (backlog item
162), which gated the cycle counter with `mcounteren = 0`; the two
experiments differ only in which counter bit is cleared. M-mode
reads are never gated, so the experiment requires a real
privilege drop to observe the gate. The module shares only
`src/boot.S` and `src/uart.c` with the other demos.

- `mtg_main.c`: M-mode setup (boot `mcounteren` readback, 0x7
  writability probe, gate write of 0x1, `mtvec`/`stvec`/`sscratch`
  install, `medeleg` bit 2 so the illegal-instruction trap is
  delivered to S-mode, a whole-address-space PMP NAPOT entry, then
  `mret` with MPP=01 into the S-mode payload). The S-mode payload
  runs phase A (labeled `rdtime` with `mcounteren.TM = 0`,
  expecting exactly one S-mode trap with `scause = 2` and `sepc`
  exactly at the rdtime site, the destination register untouched)
  and phase B (two labeled `rdcycle` samples with
  `mcounteren.CY = 1`, expecting no new trap, a strictly positive
  first sample, and a strictly increasing second sample). The
  fault sites use in-asm numeric local labels (`la t, 1f` with
  `1:` in the asm) so the assembler resolves the exact address;
  the C `&&label` construct is never used for trap-resume
  addresses. A 64-bit FNV-1a checksum is fed the eleven
  verdict-relevant values in a fixed order from both privilege
  levels and printed on the completion path. Absolute
  cycle-counter samples are deliberately excluded from the
  checksum: they are timing values that differ run to run.
- `mtg_strap.S`: S-mode trap entry (direct mode). Records
  `scause`/`stval`/`sepc`, bumps the trap counter, advances `sepc`
  by 4 (exact: `rdtime`, like `rdcycle`, has no compressed
  encoding), and `sret`.
- `mtg_mtrap.S`: M-mode trap entry (direct mode). A correct run
  takes no M-mode trap at all (no M-mode ecall is issued), so this
  handler parks the hart on any entry.
- `build.sh`, `PROOF.md` (this file), `bench-logs/` with the build
  log and three raw QEMU run logs.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 11 checks held. On PASS the machine is
shut down through the virt test-device finisher (QEMU exits 0); on
FAIL the hart parks in a `wfi` loop without touching the finisher.

Build: direct `riscv64-unknown-elf-gcc` invocations matching the
repo Makefile pattern (`-Wall -Wextra -O2 -ffreestanding
-nostdlib -nostartfiles -no-pie -fno-pie -fno-pic
-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`), logged in
`bench-logs/build.log` via `build.sh`; the Makefile itself was not
modified.
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mcounteren-time-gate.elf` under `timeout`, so a parked-hart
FAIL is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the experiment drops to S-mode via
  `mret` with `mstatus.MPP = 01`.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`),
  `riscv64-unknown-elf-gcc` 13.2.0, `-march=rv64imac_zicsr`.
- `medeleg = 0x4` (only the illegal-instruction trap delegated to
  S-mode).

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
| gate | `csrw mcounteren, 0x1`; readback | 0x1 (CY set, TM and IR clear) on all 3 runs |
| setup | `medeleg` readback | 0x4 on all 3 runs |
| phase A | S-mode `rdtime` at 0x80000288 with `mcounteren = 0x1` | trap, `scause = 0x2`, `stval = 0xc01024f3`, `sepc = 0x80000288` (exactly the rdtime site), `sepc+4 = 0x8000028c` (the resume label), trap count 1, dest register still the 0xdeadbeefdeadbeef sentinel |
| phase B | S-mode `rdcycle` with CY set | no new trap (S-mode trap count still 1), samples strictly positive and strictly increasing: deltas +8070, +8460, +9150 across the three runs |

Checks: 11 (probe readback, gate readback, medeleg bit 2,
scause == 2, sepc == fault site, sepc+4 == resume label, sentinel
intact, phase-A trap count == 1, phase-B trap count still 1, first
sample strictly positive, samples advancing). Mismatches: 0.
FNV-1a checksum over the eleven verdict-relevant values:
0xd1d0b4647fd56387, identical on all 3 runs.

Note: `stval = 0xc01024f3` is QEMU's informational readback of the
faulting `rdtime` encoding for the illegal-instruction trap; it is
printed as observed and is not part of the verdict or the
checksum.

Note on the phase-B deltas: in this QEMU build (no `-icount`) the
`cycle` counter is derived from the host clock, so two adjacent
`rdcycle` instructions report a few thousand ticks of elapsed wall
time rather than an instruction count. The verdict asserts only
strict positivity and strict increase, which is exactly what the
mechanism under test (the CY gate being open) guarantees; the
absolute values are timing noise and differ across runs.

## Limits of verification

- The +4 `sepc` advance in the S-mode handler is exact only
  because `rdtime` has no compressed encoding; the module asserts
  the result (`sepc+4` equals the labeled resume address) rather
  than assuming it.
- The gate is exercised for the TM bit only; CY gating was covered
  by `src/scounteren-trap` and IR gating (`rdinstret`) was not
  tested here.
- QEMU-specific readbacks observed but not asserted: boot
  `mcounteren = 0x0` and the `stval` encoding above.
- No restore of `mcounteren` is performed: the machine halts on
  the completion path, and M-mode restore semantics are already
  covered by `src/mcounteren`.
- U-mode gating was not tested; the module drops M-mode to S-mode
  only.
- Emulator behavior, not silicon: every number above was measured
  under QEMU 8.2.2 `virt`, not on a physical hart.
