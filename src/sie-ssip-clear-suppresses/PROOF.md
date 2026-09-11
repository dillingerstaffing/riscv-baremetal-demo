<!-- PROOF-HEADER
Checks: 20
Mismatches: 0
Checksum: 0x87a8ee03efaf53f3
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: sie.SSIE as the S-mode supervisor-software-interrupt gate (backlog item "riscv sie-ssip-clear-suppresses")

## What was built

`src/sie-ssip-clear-suppresses/`: a bare-metal RISC-V program that
verifies `sie.SSIE` gates delivery of the supervisor software
interrupt independently of the global `sstatus.SIE` gate. Four
files, sharing only `src/boot.S` and `src/uart.c` with the other
demos. Exactly one mechanism is under test: the pending `SSIP` bit
must stay pending without trapping while `SSIE` is clear (even with
`SIE` set), and must trap exactly once when `SSIE` is set.

- `ssc_trap.S`: M-mode trap entry (records `mcause`/`mepc` in
  `m_regs`, then parks; no M-mode trap is expected) and S-mode trap
  entry (records `scause`/`sepc`/the `sip` value at entry in
  `s_regs`, clears `sip.SSIP` so the level-triggered source fires
  exactly once, raises the done flag).
- `ssc_main.c`: programs the `mideleg` bit-1 delegation with
  readback checks, opens the address space with one PMP NAPOT entry,
  grants `rdcycle`/`rdtime` to S-mode via `mcounteren`, disarms
  `mie`/`mstatus.MIE`, pends `SSIP` from M-mode with `csrs mip`,
  clears `sie`, drops to S-mode via `sret` with `SIE` set, and runs
  the three phases: the gated poll (SSIE clear), the gate-open trap
  (exactly one, inside a labeled wait loop), and the quiet window
  (no re-delivery). Prints `RESULT: PASS` only when all 20 checks
  held.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make sie-ssip-clear-suppresses.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel sie-ssip-clear-suppresses.elf`
(or `make run-sie-ssip-clear-suppresses`).

Toolchain: riscv64-unknown-elf-gcc (the ~/workspace/toolchains/compat-bin
shim, pointing at xpack riscv-none-elf-gcc 15.2.0), QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`.
- The program is the only code running. The S-mode phases run after
  a `sret` drop (one PMP NAPOT entry opens the whole address space;
  `stvec`/`sscratch` installed beforehand). `medeleg` is zero
  throughout, so no exception is ever delegated. `mie` and
  `mstatus.MIE` are clear throughout the S-mode phases, so no M-mode
  interrupt can fire; `sie.SSIE` is the only interrupt enable ever
  set in S-mode.
- `mideleg` bit 1 delegates the supervisor software interrupt to
  S-mode (readback-verified: the bit takes, bit 9 stays clear).

## Sequence and controls

M-mode boot records `mideleg = 0x1444` (boot baseline), writes bit 1
and reads back `0x1446` (bit 1 takes, bit 9 clear), writes
`medeleg = 0` and reads back `0x0`, pends `SSIP` with `csrs mip`
(readback `mip = 0x82`: the SSIP bit plus an unrelated already-pending
MTIP bit, stable across all runs; MTIP cannot trap with `mie` and
`MIE` clear), clears `sie` (readback `0x0`), and `sret`s to S-mode.

Phase 1 (gate closed): S-mode enters with `SIE = 1` (verified) and
`sie = 0x0` (verified). A bounded window of 2,000,000 iterations
reads `sip` every iteration with `SIE` open: `SSIP` reads 1
throughout while the trap count stays 0. The only thing holding the
trap back is the clear `SSIE` bit.

Phase 2 (gate open): `csrsi sie, 2` inside a labeled wait loop is
the only `SSIE` 0-to-1 transition in the run. Exactly one trap
fires, `sepc = 0x800002a0` inside the loop bounds
`[0x8000029c, 0x800002b0)`, `scause = 0x8000000000000001`
(supervisor software interrupt), `sip = 0x2` at handler entry. The
handler clears `SSIP` (readback `sip = 0x0` afterwards) and the
M-mode trap count stays 0.

Phase 3 (quiet): with `SIE` and `SSIE` both on and the source
cleared, a bounded window of 2,000,000 iterations produces zero
further traps (count stays 1) and `sip.SSIP` reads 0.

## Verdict values (identical on all three runs)

- gated traps during phase 1: 0
- sip.SSIP observed during phase 1: 1
- traps after the gate opened: 1
- quiet-window extra traps: 0
- scause of the one trap: 0x8000000000000001
- sip SSIP bit after the handler: 0
- sie.SSIE read back after the gate-open: 1
- checks: 20, mismatches: 0
- FNV-1a checksum over the verdict values: 0x87a8ee03efaf53f3

All three QEMU 8.2.2 runs exited 0 (virt test-device finisher
shutdown) with byte-identical UART output; md5 of the three run
logs: `c0a41b99007218485b0fc64850f368ed` x 3.

## Defect notes

None observed in this run: every phase behaved exactly as the
specification of the per-interrupt enable predicts. One design
decision worth recording: the `SSIP` pend is done from M-mode with
`csrs mip` because prior modules measured that S-mode `sip` writes
to bit 1 are dropped on this hart (see `src/sip-ssip/`); the M-mode
pend is read back and re-verified in S-mode during phase 1, so the
module does not depend on the dropped write path.
