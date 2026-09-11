<!-- PROOF-HEADER
Checks: 14
Mismatches: 0
Checksum: 0x934b460eeaf296e0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: medeleg bit-3 breakpoint trap destination switch (backlog item "riscv medeleg-breakpoint-route")

## What was built

`src/medeleg-breakpoint/`: a bare-metal RISC-V program that executes
the same `ebreak` from S-mode in two phases and checks the trap
destination flips with `medeleg` bit 3. Phase 1 runs with `medeleg`
written to 0 (readback `0x0` on QEMU 8.2.2, bit 3 verified clear):
the ebreak must trap in M-mode with `mcause = 3` and `mepc` exactly
at the ebreak. Phase 2 runs with bit 3 set (readback exactly
`0x8`): the same ebreak must trap in S-mode with `scause = 3` and
`sepc` exactly at the ebreak. Four files, sharing only `src/boot.S`
and `src/uart.c` with the other demos. Exactly one mechanism is
under test: the destination of a supervisor-mode breakpoint trap as
`medeleg` bit 3 flips.

- `mebk_trap.S`: M-mode trap entry (records `mcause`/`mepc`/
  `mstatus`/`mtval` in `m_regs`; the only M-mode trap possible is the
  phase-1 ebreak, so the handler then loads the phase-2 continuation
  from its save area into `mepc`, sets `mstatus.MPP` to M-mode, and
  `mret`s into it) and S-mode trap entry (records `scause`/`sepc`/
  `sstatus`/`stval` in `s_regs`, advances `sepc` by 4 past the
  ebreak, raises the done flag). Zero continuation or unplanned trap
  parks the hart.
- `mebk_main.c`: two-phase driver. Writes `medeleg`, verifies the
  readback, drops M -> S via `sret`, executes the ebreak from
  S-mode in both phases, publishes every measured value, runs the
  checks, computes an FNV-1a checksum over the verdict values, and
  reports PASS/FAIL. On PASS it writes `0x5555` to the virt
  test-device finisher so the QEMU process exits 0; on FAIL it
  parks the hart.

## How it was verified

- Built once with the repo Makefile (default `riscv64-unknown-elf-`
  toolchain): build log in `bench-logs/build.log`.
- Ran 3 times under QEMU 8.2.2 (`-machine virt -nographic -bios
  none -kernel`); raw console logs in `bench-logs/run1.log`,
  `run2.log`, `run3.log`. All three runs exit 0 and are
  byte-identical.
- Each run executes 14 checks and records 0 mismatches. The FNV-1a
  checksum over the verdict values
  (`m_traps`, `mcause`, `mepc`, `s_traps`, `scause`, `sepc`,
  both `medeleg` readbacks) is `0x934b460eeaf296e0` in all three
  runs.

## Measured values (identical across all 3 runs)

- Boot `medeleg`: `0x0`.
- Phase 1: wrote `0x0`, read back `0x0` (bit 3 clear). M-mode
  traps = 1, `mcause = 0x3`, `mepc = 0x800002d4` exactly matching
  the ebreak address, `mstatus.MPP = 1` (trap arrived from S-mode),
  `mtval = 0x0`, S-mode traps = 0.
- Phase 2: set bit 3, read back `0x8` (bit 3 takes, no forced extra
  bits). S-mode traps = 1, `scause = 0x3`, `sepc = 0x80000392`
  exactly matching the ebreak address, `sstatus.SPP = 1` (trap
  arrived from S-mode), `stval = 0x0`, M-mode traps still = 1 (no
  trap leaked into M-mode in phase 2).
- Quiet window (2,000,000 spins with all interrupt enables clear):
  both trap counts unchanged.

## Notes

- The ebreak is emitted as `.word 0x00100073` (the 32-bit SYSTEM
  encoding), not the `ebreak` mnemonic: under RVC the mnemonic
  assembles to the 2-byte `c.ebreak`, while the S-mode handler
  resumes exactly 4 bytes past the trap site. The first build used
  the mnemonic and hung with zero UART output (phase 2 resumed
  mid-instruction); the disassembly showed `9002 c.ebreak` and the
  fix was verified in the object file before the passing runs.
- `mtval`/`stval = 0x0` is what QEMU 8.2.2 reports for a
  breakpoint trap; unlike the illegal-instruction case (which
  reports the faulting word), the breakpoint trap value carries no
  address. The values are published, not asserted.
- Distinct from `src/medeleg-illegal-inst-route` (bit 2, illegal
  instruction) and `src/medeleg-ecall-destination` (bit 9, S-mode
  ecall): this routes the breakpoint trap on bit 3.
