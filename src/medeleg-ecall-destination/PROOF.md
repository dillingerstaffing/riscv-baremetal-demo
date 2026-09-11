<!-- PROOF-HEADER
Checks: 14
Mismatches: 0
Checksum: 0x5ce302e5b39ef6f0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: medeleg bit-9 S-mode ecall destination switch (backlog item "riscv medeleg-ecall-destination")

## What was built

`src/medeleg-ecall-destination/`: a bare-metal RISC-V program that
issues the same `ecall` from S-mode in two phases and checks the
trap destination flips with `medeleg` bit 9. Phase 1 runs with
`medeleg` zeroed: the ecall must trap in M-mode with
`mcause = 9` and `mepc` at the ecall. Phase 2 runs with bit 9 set:
the ecall must trap in S-mode with `scause = 9` and `sepc` at the
ecall. Four files, sharing only `src/boot.S` and `src/uart.c`
with the other demos. Exactly one mechanism is under test: the
destination of a supervisor environment call as `medeleg` bit 9
flips.

- `mede_trap.S`: M-mode trap entry (records `mcause`/`mepc`/
  `mstatus` in `m_regs`; the only M-mode trap possible is the
  phase-1 ecall, so the handler then loads the phase-2
  continuation from its save area into `mepc`, sets
  `mstatus.MPP` to M-mode, and `mret`s into it) and S-mode trap
  entry (records `scause`/`sepc`/`sstatus` in `s_regs`,
  advances `sepc` by 4 past the ecall, raises the done flag).
- `mede_main.c`: installs direct-mode `mtvec`/`mscratch` and
  `stvec`/`sscratch`, records the boot `medeleg`, programs the
  per-phase delegation with readback checks, opens the address
  space to S-mode with one PMP NAPOT entry, disarms `mie`/
  `mstatus.MIE`, drops to S-mode for phase 1, runs the phase-2
  setup on the M-mode handler's redirect, drops to S-mode for
  phase 2, then prints every measured value, runs the 14 checks,
  takes a quiet window, prints an FNV-1a checksum over the
  verdict values, and prints `RESULT: PASS` only when all 14
  checks held.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make medeleg-ecall-destination.elf` (added to `all` in the
Makefile; `src/boot.S` stays first in `MEDE_SRCS` so `_start`
lands at `0x80000000`, the address QEMU's `-kernel` loader
starts at).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel medeleg-ecall-destination.elf`
(or `make run-medeleg-ecall-destination`).

Toolchain: xPack GNU RISC-V Embedded GCC 15.2.0
(`riscv64-unknown-elf-gcc` via `~/workspace/toolchains/xv6-rv64`),
QEMU 8.2.2 (`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`.
- The program is the only code running. Both test phases run in
  S-mode after `sret` drops (one PMP NAPOT entry opens the whole
  address space; `stvec`/`sscratch` installed beforehand). `mie`,
  `mstatus.MIE`, `sie`, and `sstatus.SIE` are clear for the whole
  run, so no interrupt of either kind can fire; the only traps
  are the two ecalls.
- The S-mode payloads capture their ecall addresses with
  in-assembly local labels before the ecall executes. (Phase 1's
  payload is never resumed, so a store placed after its ecall in
  C could never run; the address is stored from inside the asm
  block. Phase 2's payload is resumed by its handler's
  `sepc + 4` skip.)

## Sequence and controls

1. Record boot-time `medeleg` (`0x0`).
2. Write `0` to `medeleg`, read back: must be `0x0` with bit 9
   clear. This is the delegation state phase 1 runs under.
3. Phase 1: drop to S-mode, issue `ecall`. Require exactly one
   M-mode trap with `mcause = 0x9` (environment call from
   S-mode), `mepc` equal to the captured ecall address,
   `mstatus.MPP = 1` (the trap arrived from S-mode), and zero
   S-mode traps. The M-mode handler redirects to the phase-2
   M-mode setup.
4. Phase 2: write `0x200` to `medeleg`, read back: bit 9 must be
   set (`0x200`). Snapshot the phase-1 trap counts, drop to
   S-mode, issue `ecall`. Require exactly one S-mode trap with
   `scause = 0x9`, `sepc` equal to the captured ecall address,
   `sstatus.SPP = 1`, and the M-mode trap count still 1.
5. Quiet window: poll with nothing pending; require the counts
   unchanged. Print `RESULT: PASS` only if all 14 checks held.

## Measured results

All quantities below are identical across the three runs
(byte-identical UART payloads; the logs differ only in the
harness timeout line that `timeout(1)` appends, which carries the
QEMU pid):

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| boot `medeleg` | `0x0` | identical | identical |
| phase-1 `medeleg` write `0x0` readback | `0x0` | identical | identical |
| phase-2 `medeleg` write `0x200` readback | `0x200` | identical | identical |
| phase-1: M-mode traps / `mcause` | 1 / `0x9` | identical | identical |
| phase-1: `mepc` / captured ecall address | `0x800002c4` / `0x800002c4` | identical | identical |
| phase-1: `mstatus.MPP` / S-mode traps | 1 / 0 | identical | identical |
| phase-2: S-mode traps / `scause` | 1 / `0x9` | identical | identical |
| phase-2: `sepc` / captured ecall address | `0x80000370` / `0x80000370` | identical | identical |
| phase-2: `sstatus.SPP` / M-mode traps | 1 / 1 | identical | identical |
| quiet: M-mode / S-mode traps | 1 / 1 | identical | identical |
| FNV-1a checksum over the verdict values | `0x5ce302e5b39ef6f0` | identical | identical |
| verdict | PASS | PASS | PASS |

The 14 checks: (1) phase-1 `medeleg` readback zero,
(2) phase-1 readback bit 9 clear, (3) phase-1 M-mode trap fired
exactly once, (4) phase-1 `mcause = 0x9`, (5) phase-1 `mepc`
equals the ecall address, (6) phase-1 `mstatus.MPP = 1`,
(7) phase-1 S-mode traps zero, (8) phase-2 `medeleg` readback
has bit 9 set, (9) phase-2 S-mode trap fired exactly once,
(10) phase-2 `scause = 0x9`, (11) phase-2 `sepc` equals the
ecall address, (12) phase-2 `sstatus.SPP = 1`,
(13) no trap leaked into the wrong mode's handler (final
M-mode traps 1, S-mode traps 1), (14) quiet window counts
unchanged. Zero failures on all three runs.

## The 0xb vs 0x9 cause-code note

The backlog item as written names `mcause = 0xb` for the
phase-1 trap. `0xb` is the environment call from M-mode; an
`ecall` issued in S-mode traps with cause 9 (environment call
from S-mode) whether or not `medeleg` delegates it. That is what
the RISC-V privileged specification assigns, what QEMU 8.2.2
delivers, and what the pre-existing `src/medeleg-ecall` module
in this repo already measured. The checks in this module assert
`0x9`, not `0xb`; asserting `0xb` would have failed honestly.

## The mret/MPP redirect

The M-mode handler does not resume the trapped S-mode payload;
it loads the phase-2 setup address into `mepc` and `mret`s into
it. `mret` takes the target privilege from `mstatus.MPP`, and a
trap from S-mode leaves `MPP = 01`, so the handler sets
`MPP = 11` (M-mode) before the `mret`. Without that step the
return drops back into S-mode, the phase-2 `csrw medeleg`
raises an illegal-instruction trap (an M-mode-only CSR touched
from S-mode), the handler finds a zero continuation, and the
hart parks silently; this exact hang was observed and fixed
during the build.

## Raw QEMU output

### Run 1 (bench-logs/run1.log)

```
medeleg-ecall-destination: S-mode ecall destination switch test
boot: medeleg=0x0
phase1: medeleg-write=0x0 readback=0x0
phase2: medeleg-write=0x200 readback=0x200
p1: m_traps=1 mcause=0x9 mepc=0x800002c4 expected=0x800002c4 mstatus_mpp=1 s_traps=0
p2: s_traps=1 scause=0x9 sepc=0x80000370 expected=0x80000370 sstatus_spp=1 m_traps=1
quiet: m_traps=1 s_traps=1
checksum=0x5ce302e5b39ef6f0
RESULT: PASS
done
```

Runs 2 and 3 carry the identical UART payload; their logs differ
only in the `timeout(1)` termination line, which embeds the QEMU
process id.
