<!-- PROOF-HEADER
Checks: 13
Mismatches: 0
Checksum: 0xd9b098bdf45ff160
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: medeleg bit-2 illegal-instruction trap destination switch (backlog item "riscv medeleg-illegal-inst-route")

## What was built

`src/medeleg-illegal-inst-route/`: a bare-metal RISC-V program that
executes the same `0xffffffff` illegal word from S-mode in two phases
and checks the trap destination flips with `medeleg` bit 2. Phase 1
runs with `medeleg` written to 0 (readback `0x0` on QEMU 8.2.2, bit 2
verified clear): the illegal instruction must trap in M-mode with
`mcause = 2` and `mepc` exactly at the illegal word. Phase 2 runs
with bit 2 set (readback `0x4`): the same word must trap in S-mode
with `scause = 2` and `sepc` exactly at the word. Four files, sharing
only `src/boot.S` and `src/uart.c` with the other demos. Exactly one
mechanism is under test: the destination of a supervisor-mode
illegal-instruction trap as `medeleg` bit 2 flips.

- `mili_trap.S`: M-mode trap entry (records `mcause`/`mepc`/
  `mstatus`/`mtval` in `m_regs`; the only M-mode trap possible is the
  phase-1 illegal instruction, so the handler then loads the phase-2
  continuation from its save area into `mepc`, sets
  `mstatus.MPP` to M-mode, and `mret`s into it) and S-mode trap
  entry (records `scause`/`sepc`/`sstatus`/`stval` in `s_regs`,
  advances `sepc` by 4 past the illegal word, raises the done
  flag). Zero continuation or unplanned trap parks the hart.
- `mili_main.c`: two-phase driver. Writes `medeleg`, verifies the
  readback, drops M -> S via `sret`, executes the illegal word from
  S-mode in both phases, publishes every measured value, runs the
  checks, computes an FNV-1a checksum over the verdict values, and
  reports PASS/FAIL. On PASS it writes `0x5555` to the virt
  test-device finisher so the QEMU process exits 0; on FAIL it
  parks the hart.

## How it was verified

- Built once with the repo Makefile (CROSS override to the
  compat-bin xpack 15.2.0 toolchain): build log in
  `bench-logs/build.log`.
- Ran 3 times under QEMU 8.2.2 (`-machine virt -nographic -bios
  none -kernel`); raw console logs in `bench-logs/run1.log`,
  `run2.log`, `run3.log`. All three runs exit 0 and are
  byte-identical.
- Each run executes 13 checks and records 0 mismatches. The FNV-1a
  checksum over the verdict values
  (`m_traps`, `mcause`, `mepc`, `s_traps`, `scause`, `sepc`,
  both `medeleg` readbacks) is `0xd9b098bdf45ff160` in all three
  runs.

## Measured values (identical across all 3 runs)

- Boot `medeleg`: `0x0`.
- Phase 1: wrote `0x0`, read back `0x0` (bit 2 clear). M-mode
  traps = 1, `mcause = 0x2`, `mepc = 0x800002d8` exactly matching
  the illegal-word address, `mstatus.MPP = 1` (trap arrived from
  S-mode), `mtval = 0xffffffff` (the hart reported the faulting
  word itself), S-mode traps = 0.
- Phase 2: set bit 2, read back `0x4` (bit 2 set). S-mode traps =
  1, `scause = 0x2`, `sepc = 0x8000039e` exactly matching the
  illegal-word address, `sstatus.SPP = 1` (trap arrived from
  S-mode), `stval = 0xffffffff`, M-mode traps still = 1 (no
  trap leaked into M-mode in phase 2).
- Quiet window (2,000,000 spins with all interrupt enables clear):
  both trap counts unchanged.

## Notes

- The backlog's premise said a write-0 `medeleg` would read back
  `0x1444` on QEMU 8.2.2. That value is what QEMU 8.2.2 boots
  `mideleg` with (forced bits), not `medeleg`: this run observes
  boot `medeleg = 0x0`, a write-0 readback of `0x0`, and a bit-2
  write readback of `0x4`. The shipped result reflects the
  observed values.
- `mtval`/`stval = 0xffffffff` is what QEMU 8.2.2 reports for an
  illegal-instruction trap: the faulting word. This corroborates
  that the trap fired on exactly the word placed at the labeled
  site.
