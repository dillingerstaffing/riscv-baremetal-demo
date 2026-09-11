<!-- PROOF-HEADER
Checks: 26
Mismatches: 0
Checksum: 0xaee064c41add58e2
Environment: QEMU 8.2.2
Verdict: PASS
-->
# Proof: S-mode sip.STIP write reads back identically with mideleg bit 5 clear and set (backlog item riscv sip-stip-mideleg-reconcile)

## The question

Two earlier modules disagreed on the surface about the S-mode
software write to `sip` bit 5 (STIP). The `sip-stip-write` module
measured `csrs sip, 0x20` in S-mode with STI delegated reading back
`0x0`, while the `sip-seip-write` module measured the S-mode sip
write path as dropped entirely when the bit is not delegated via
`mideleg`. The question this module answers: does raising
`mideleg` bit 5 (STI delegation) change what an S-mode all-ones
`sip` write does to bit 5? The measurement: no. With bit 5 clear
the write reads back `0x2` (SSIP only, the delegated bit); with bit
5 set the write reads back `0x2` as well. STIP is legalized away in
both configurations, and the reconciliation check in the program
requires the STIP-stuck flag to be identical in the two
configurations. The premises are reconciled: both siblings are
consistent, because the delegation gate controls which bits an
S-mode write can admit, and on this hart STIP is never admitted
either way.

## What was built

`src/sip-stip-mideleg-reconcile/`: a bare-metal RISC-V program that
probes the same S-mode all-ones `sip` write twice in one boot, with
a deliberate `ecall` between the phases so the M-mode handler can
raise `mideleg` bit 5 mid-run. Three files, sharing only
`src/boot.S` and `src/uart.c` with the other demos. Exactly one
question is under test: STIP writability with `mideleg` bit 5
clear vs set.

- `strm_trap.S`: the only trap vector, M-mode direct. Entry swaps
  `t0` with `mscratch`, parks `t1`/`t2`/`t3`, bumps a trap count,
  records `mcause`/`mepc`, and advances `mepc` past the trapping
  instruction (2 or 4 bytes) so a surprise trap cannot loop
  silently. It recognizes the deliberate end-of-Phase-A `ecall`
  exactly (count 1, `mcause` 9): only then it sets `mideleg` bit 5,
  records the new `mideleg` readback in the `mideleg_phase_b`
  global, and raises the `phase_b` flag. Any other trap is only
  recorded and skipped past; the S-mode checks then fail loudly.
  `mstatus.MPP` is S on trap entry, so `mret` resumes in S-mode at
  the advanced `mepc`.
- `strm_main.c`: M-mode boot (UART, counting vector, machine timer
  comparator parked at all-ones, Sstc probe via `menvcfg.STCE`,
  one PMP NAPOT R/W/X entry, `mcounteren`, `mideleg` set to
  delegate SSI only for Phase A, then `mret` to S-mode) and the
  S-mode payload: Phase A entry checks (`sstatus.SIE` clear,
  `sie` zero, `sip` zero), an all-ones write with readback and
  stuck flags, a zero write with readback, the deliberate `ecall`,
  verification of the transition (trap count exactly 1,
  `mcause` 9, `mideleg` now exactly SSI+STI delegated), then Phase
  B repeating the writes, and finally the reconciliation check that
  the STIP-stuck flag is identical in both phases. In each phase
  the exact legalized readback is asserted from the observed stuck
  flags (only bits observed writable may differ from the before
  value), and SSIP sticking proves the write executed, so a clear
  STIP is legalized away, not a dropped write. A failed check
  prints `FAIL` and increments the fails counter; `RESULT: PASS`
  is printed only when every check held. The verdict-relevant
  values feed a 64-bit FNV-1a digest printed as the last data line,
  so the six bench runs can be compared for byte-identical output.
  On PASS the machine is shut down through the virt test-device
  finisher (QEMU exits 0); on FAIL the hart parks without touching
  the finisher.
- `PROOF.md` (this file), `bench-logs/` with the build log and six
  raw QEMU run logs.

Build: `make sip-stip-mideleg-reconcile.elf` (added to `all` in the
Makefile). Run: `timeout 30 qemu-system-riscv64 -machine virt
-nographic -bios none -kernel sip-stip-mideleg-reconcile.elf` (or
`make run-sip-stip-mideleg-reconcile`).

Toolchain: `riscv64-unknown-elf-gcc` 13.2.0,
`-march=rv64imac_zicsr`. QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: hart 0, single hart, M-mode at boot (`-bios none`),
  S-mode for the payload after the module's own `mret` drop.
- Sstc present: `menvcfg.STCE` sticks when set, probed at boot.
- Phase A `mideleg` readback `0x1446`: SSI delegated, STI and SEI
  clear. The stuck bits 2, 6, 8, 10 are this emulator's reset value
  (`0x1444`, virtualization-related, not clearable by the module's
  write); the check asserts only the bits that matter.
- Phase B `mideleg` readback `0x1466`: the handler raised exactly
  bit 5 on top of the Phase A delegation, verified by readback.
- `sie` and `sstatus.SIE` stay clear for the whole run, so no
  pending bit can be taken as an interrupt; the trap count tracks
  only the deliberate `ecall`.

## Measured results (6 QEMU runs, byte-identical)

Raw logs: `bench-logs/run1.log` through `run6.log` (md5
`070be67c7f01970ea21b2062f07b4bc3` for all six). Build log:
`bench-logs/build.log`. All six runs printed byte-identical
output, `checks: 26 fails: 0`,
`checksum: 0xaee064c41add58e2`, `RESULT: PASS`, and QEMU exited 0
via the test-device finisher on every run.

| step | all 6 runs |
|---|---|
| `menvcfg.STCE` probe | sticks (present) |
| Phase A `mideleg` | 0x1446 (bit 5 clear, SSI delegated) |
| `sie` at S-mode entry | 0x0 |
| `sstatus.SIE` at entry | clear |
| Phase A `sip` before | 0x0 |
| Phase A `sip` after all-ones | 0x2 (SSIP stuck, STIP clear) |
| Phase A trap count | 0 |
| Phase A `sip` after zero | 0x0 |
| trap count after `ecall` | 1 |
| recorded `mcause` | 0x9 (S-mode ecall) |
| Phase B `mideleg` | 0x1466 (bit 5 set, SSI+STI delegated) |
| Phase B `sip` before | 0x0 |
| Phase B `sip` after all-ones | 0x2 (SSIP stuck, STIP clear) |
| Phase B trap count | 1 |
| Phase B `sip` after zero | 0x0 |
| reconciliation: STIP-stuck A vs B | 0 vs 0, identical |
| checks / fails | 26 / 0 |
| FNV-1a checksum of verdict values | 0xaee064c41add58e2 |
| RESULT | PASS |

What the readbacks ground:

- `sip = 0x2` after the S-mode all-ones write in BOTH
  configurations: bit 1 (SSIP) is software-writable and stuck,
  which proves the write executed in each phase; bit 5 (STIP)
  reads back clear in each phase. Delegating STI (raising
  `mideleg` bit 5) does not change the S-mode write's effect on
  STIP: the bit is legalized away whether or not the interrupt is
  delegated.
- The write is not dropped wholesale in either phase: SSIP
  sticking in each phase is the write-executed proof (this QEMU
  admits an S-mode `sip` write only for bits whose interrupt is
  delegated, per the `sip-seip-write` sibling, and SSI is
  delegated in both phases here).
- The zero writes clear the writable bits (`0x0` both phases)
  with no trap, and the single recorded trap is the deliberate
  S-mode `ecall` (`mcause` 9) that performed the phase transition.
- The checksum covers both `mideleg` readbacks, the entry `sie`,
  the write values, all six readbacks, both stuck-flag pairs, the
  final trap count, and the recorded `mcause`. It is identical
  across runs because every measured value is identical.

## What was verified, and what was not

Verified: on QEMU 8.2.2, an S-mode `csrw sip, -1` with
`mideleg` bit 5 clear reads back `0x2`, and with bit 5 set also
reads back `0x2`; STIP does not stick in either configuration,
and the module's reconciliation check requires the STIP-stuck
flag to be identical in the two. A `csrw sip, 0` clears the
writable bits in both configurations; exactly one trap (the
deliberate `ecall`, `mcause` 9) fires across the whole run; six
runs were byte-identical (digest `0xaee064c41add58e2`).

Not verified: behavior on real silicon. These numbers come from
the QEMU 8.2.2 CSR model, not from hardware. Also not verified:
any path where an S-mode STIP write could stick (it cannot on
this hart in either delegation configuration).

## Limits

- The module assumes the `mideleg` writes, the PMP opening, the
  SIE clear, and the `sie` zero state hold exactly as written
  (each checked via readback; the run fails loudly if any does
  not).
- The `mideleg` stuck bits 2, 6, 8, 10 (`0x1444`) are this
  emulator's reset value; the checks assert only the bits that
  matter (SSI, STI, SEI).
- The "write executed" proof rests on SSIP sticking, which on
  this QEMU requires the SSI delegation; the delegation is the
  minimal change that makes the write observable, and the STIP
  bit under test is the only variable between the phases.

## Reproduction

```
make sip-stip-mideleg-reconcile.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel sip-stip-mideleg-reconcile.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.

## Build log (bench-logs/build.log, verbatim)

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sip-stip-mideleg-reconcile/strm_trap.S -o src/sip-stip-mideleg-reconcile/strm_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sip-stip-mideleg-reconcile/strm_main.c -o src/sip-stip-mideleg-reconcile/strm_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o sip-stip-mideleg-reconcile.elf src/boot.o src/uart.o src/sip-stip-mideleg-reconcile/strm_trap.o src/sip-stip-mideleg-reconcile/strm_main.o
/home/hatch/workspace/toolchains/xpack-riscv-none-elf-gcc-15.2.0-1/bin/../lib/gcc/riscv-none-elf/15.2.0/../../../../riscv-none-elf/bin/ld: warning: sip-stip-mideleg-reconcile.elf has a LOAD segment with RWX permissions
```

(The linker prints its usual `LOAD segment with RWX permissions`
warning, as for every module in this repo.)

## Raw run output (run1; runs 2 through 6 are byte-identical)

```
sip-stip-mideleg-reconcile: STIP write vs mideleg bit 5
Sstc probe: menvcfg.STCE sticks (present)
phaseA mideleg=0x1446
dropping to S-mode
in S-mode: Phase A, mideleg bit 5 (STI) CLEAR; SSI delegated
sie at entry=0x0
phaseA sip before=0x0
phaseA writing all-ones to sip from S-mode
phaseA sip after all-ones=0x2
phaseA SSIP-stuck=1 STIP-stuck=0
phaseA M-mode trap count=0 (expect 0)
phaseA writing zero to sip from S-mode
phaseA sip after zero=0x0
phaseA done; ecall to M-mode to delegate STI for phase B
phase B mideleg=0x1466
M-mode trap count after the ecall=1 (expect 1)
recorded mcause=0x9 (expect 9: S-mode ecall)
phaseB sip before=0x0
phaseB writing all-ones to sip from S-mode
phaseB sip after all-ones=0x2
phaseB SSIP-stuck=1 STIP-stuck=0
phaseB M-mode trap count=1 (expect 1)
phaseB writing zero to sip from S-mode
phaseB sip after zero=0x0
reconciliation: STIP-stuck phaseA=0 phaseB=0
VERDICT mideleg_A=0x1446 mideleg_B=0x1466 sipA_after=0x2 stipA_stuck=0 sipB_after=0x2 stipB_stuck=0 traps=1 checksum=0xaee064c41add58e2
checks=26 fails=0
RESULT: PASS
```
