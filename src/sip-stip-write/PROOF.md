<!-- PROOF-HEADER
Checks: 8
Mismatches: 0
Checksum: 0x778b1a14b6876aa7
Environment: QEMU 8.2.2
Verdict: PASS
-->
# Proof: sip.STIP software write in S-mode is legalized away on QEMU 8.2.2 (backlog item riscv sip-stip-write)

## Note on the item's premise

The backlog item expected `sip` bit 5 (STIP) to be writable in
S-mode when the Sstc extension is present, so that software could
pend its own supervisor timer interrupt. The measurement on QEMU
8.2.2's `virt` machine disproves that expectation: `csrs sip, 0x20`
in S-mode, with `menvcfg.STCE` confirmed stuck and the supervisor
timer interrupt delegated via `mideleg` bit 5, reads back `0x0`.
The write is legalized away; STIP never sticks. This module ships
the corrected, measured finding, not the expected one. The verdict
logic below asserts what was measured. (Precedent: `src/mcause-warl`
shipped a "backlog premise NOT reproduced" finding as a real
result.)

## What was built

`src/sip-stip-write/`: a bare-metal RISC-V program that measures the
legalized value of `sip` after a software STIP write in S-mode, on
the QEMU `virt` board. Three files, sharing only `src/boot.S` and
`src/uart.c` with the other demos. Exactly one mechanism is under
test: whether the `csrs sip, 0x20` write sticks in S-mode with Sstc
present.

- `ssw_trap.S`: S-mode trap entry recording scause/sepc and calling
  the C handler `ssw_trap_handler()` on a dedicated trap stack. The
  handler is built for the premise path: on the first trap it would
  record scause/sepc, clear STIP with `csrc sip, 0x20`, and publish
  the sip readback after the clear. Because the write never stuck,
  this path never fired in any run; the counter must stay 0. The
  M-mode vector `m_trap_entry` is a minimal record-and-park handler;
  no M-mode trap is expected after boot, and one would show up in
  the log as a FAIL rather than a silent hang.
- `ssw_main.c`: M-mode boot (probe Sstc via `menvcfg.STCE`, disarm
  both comparators, open one PMP NAPOT entry, grant the cycle/time
  counters via `mcounteren`, delegate the supervisor timer interrupt
  via `mideleg` bit 5, install direct-mode `stvec`, `mret` into
  S-mode) and the S-mode payload: disarm `stimecmp` to all-ones
  FIRST so no real timer can fire, enable `sie.STIE`, keep
  `sstatus.SIE` clear, issue `csrs sip, 0x20`, and read back `sip`.
  If STIP does not stick (the measured outcome), the module takes
  the honest fork: it publishes the readback, asserts the trap
  counter stays at 0, and runs a bounded 200,000-`rdcycle` quiet
  window requiring zero traps, since a pending bit that never
  pended can produce no delivery. If STIP had stuck, the module
  would instead open `sstatus.SIE` inside a labeled wait region and
  require exactly one trap with scause `0x8000000000000005` and
  `sepc` inside the region, followed by a no-re-delivery quiet
  window. A failed check prints `FAIL` and increments the mismatches
  counter; `RESULT: PASS` is printed only when every check held. The
  verdict-relevant values feed a 64-bit FNV-1a digest printed as
  the last data line, so the three bench runs can be compared for
  byte-identical output. On PASS the machine is shut down through
  the virt test-device finisher (QEMU exits 0); on FAIL the hart
  parks without touching the finisher.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make sip-stip-write.elf` (added to `all` in the Makefile).
Run: `timeout 30 qemu-system-riscv64 -machine virt -nographic
-bios none -kernel sip-stip-write.elf` (or
`make run-sip-stip-write`).

Toolchain: Debian `riscv64-unknown-elf-gcc` 13.2.0,
`-march=rv64imac_zicsr`. QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: hart 0, single hart, M-mode at boot (`-bios none`), S-mode
  for the payload after the module's own `mret` drop.
- Sstc present: `menvcfg.STCE` sticks when set, probed at boot, so
  the S-mode `stimecmp` disarm is legal.
- `mideleg` readback `0x1464`: the supervisor timer interrupt bit 5
  took (bit 5 is set, readable and writable), so STI is delegated to
  S-mode and an STIP pending bit would have trapped as
  `0x8000000000000005`.
- The only enabled S-mode interrupt is `sie.STIE`; `sstatus.SIE`
  stays clear in the fork taken, so no trap is possible from any
  source the run does not create.

## Measured results (3 QEMU runs, byte-identical)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log` (md5
`00c1e470b15799308aa894458be19a9e` for all three). Build log:
`bench-logs/build.log`. All three runs printed byte-identical
output, `checks: 8 mismatches: 0`,
`checksum: 0x778b1a14b6876aa7`, `RESULT: PASS (premise not reproduced:
write ignored)`, and QEMU exited 0 via the test-device finisher on
every run.

| step | run1 | run2 | run3 |
|---|---|---|---|
| `menvcfg.STCE` probe | sticks (present) | sticks (present) | sticks (present) |
| `mideleg` readback | 0x1464 (STI bit took) | 0x1464 | 0x1464 |
| `stimecmp` at S-mode entry | 0xffffffffffffffff | 0xffffffffffffffff | 0xffffffffffffffff |
| `sie.STIE` readback | set | set | set |
| `sstatus.SIE` before write | clear | clear | clear |
| `csrs sip, 0x20` readback | 0x0 (STIP clear) | 0x0 | 0x0 |
| traps before quiet window | 0 | 0 | 0 |
| quiet window 200000 rdcycle reads | 0 traps | 0 traps | 0 traps |
| checks / mismatches | 8 / 0 | 8 / 0 | 8 / 0 |
| FNV-1a checksum of verdict values | 0x778b1a14b6876aa7 | 0x778b1a14b6876aa7 | 0x778b1a14b6876aa7 |
| RESULT | PASS | PASS | PASS |

What the readbacks ground:

- `sip = 0x0` after `csrs sip, 0x20` in S-mode with Sstc present and
  STI delegated: bit 5 is not software-writable on this hart; the
  write is legalized away. The pending bit never exists, so the
  delivery mechanism (trap on SIE set) has nothing to deliver, and
  the 200,000-read quiet window with 0 traps is the measured
  consequence.
- The checksum covers the sip readback after the write, the
  STIP-stuck flag (0), the trap count (0), the recorded scause
  (0, no trap), the handler's after-clear readback (0, handler
  never ran), the quiet-window extra-trap count (0), and the SIE
  readback (0). It is identical across runs because every measured
  value is identical.

## What was verified, and what was not

Verified: on QEMU 8.2.2, a `csrs sip, 0x20` in S-mode with Sstc
present and STI delegated reads back `0x0`; no trap fires and none
can, since the pending bit never pends; three runs were
byte-identical (digest `0x778b1a14b6876aa7`).

Not verified: behavior on real silicon. These numbers come from the
QEMU 8.2.2 CSR model, not from hardware. Also not verified:
delivery of a software-pended supervisor timer interrupt, since the
write that would have pended it is ignored; the trap-entry,
handler-clear, and labeled-wait-region code in this module is
compiled but unexercised by design, and the quiet-window evidence
is about absence, which is exactly what the measured WARL result
supports.

## Limits

- The module assumes the `stimecmp` disarm, the `sie.STIE` set, and
  the SIE-clear state hold exactly as written (each checked via
  readback, and the run fails loudly if any does not).
- The measured `mideleg` boot/delegation value `0x1464` is this
  emulator's choice; the module sets only bit 5 and checks the
  readback bit rather than assuming the rest.
- The 200,000-`rdcycle` quiet window is bounded by construction; a
  trap from the software write would have been visible as a counter
  change inside it.

## Reproduction

```
make sip-stip-write.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel sip-stip-write.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
