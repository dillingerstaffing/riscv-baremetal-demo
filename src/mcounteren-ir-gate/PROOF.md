<!-- PROOF-HEADER
Checks: 17
Mismatches: 0
Checksum: 0xdd2308106691e2e3
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mcounteren gates the S-mode instret counter independently of the cycle and time counters

## What was built

`src/mcounteren-ir-gate/`: a bare-metal RISC-V program that
verifies, on the QEMU `virt` board, that `mcounteren.IR` (bit 2)
gates S-mode reads of `instret` independently of the CY and TM
bits: with `mcounteren = 0x3` (CY and TM set, IR clear) an S-mode
`rdinstret` raises an illegal-instruction exception instead of
returning a count, while an S-mode `rdcycle` at the same privilege
level still reads and advances. The gate is then reopened from
M-mode (`mcounteren = 0x7`) after a real return to M-mode via
`ecall`, and two S-mode `rdinstret` reads succeed with strictly
increasing samples. This is the instret-counter counterpart of
`src/mcounteren-time-gate` (backlog item 170), which gated the
time counter with `mcounteren = 0x1`, and of `src/scounteren-trap`,
which gated the cycle counter; the three experiments differ only
in which counter bit is cleared. M-mode reads are never gated, so
the experiment requires a real privilege drop to observe the gate,
and S-mode cannot write `mcounteren`, so the second half requires
a real return to M-mode to reopen the gate. The module shares
only `src/boot.S` and `src/uart.c` with the other demos.

- `mir_main.c`: M-mode setup (boot `mcounteren` readback, 0x7
  writability probe, gate write of 0x3, `mtvec`/`stvec` install,
  `sscratch`/`mscratch` save areas, `medeleg` bit 2 so the
  illegal-instruction trap is delivered to S-mode while the
  S-mode `ecall` stays undelegated and reaches M-mode, a
  whole-address-space PMP NAPOT entry, then `mret` with MPP=01
  into the S-mode phase-A payload). Phase A runs a labeled
  `rdinstret` with `mcounteren.IR = 0` (expecting exactly one
  S-mode trap with `scause = 2`, `sepc` exactly at the rdinstret
  site, and the destination register untouched), then one labeled
  `rdcycle` with `mcounteren.CY = 1` (expecting no new trap and a
  strictly positive sample), then records the `ecall` site address
  and issues `ecall`. Phase B verifies the M-mode record
  (`mcause = 9`, `mepc` exactly at the recorded `ecall` site,
  exactly one M-mode trap, `mcounteren` readback exactly 0x7),
  then runs two labeled `rdinstret` reads with the IR bit set
  (expecting no new S-mode trap, a strictly positive first
  sample, and a strictly greater second sample, separated by a
  short spin loop so the increase holds regardless of the
  counter's time base). The fault sites use in-asm numeric local
  labels (`la t, 1f` with `1:` in the asm) so the assembler
  resolves the exact address; the C `&&label` construct is never
  used for trap-resume addresses. The `ecall` site address is
  stored to memory inside the same asm block before the `ecall`
  executes, because the trap means no instruction after the
  `ecall` ever runs in phase A. A 64-bit FNV-1a checksum is fed
  the seventeen verdict-relevant values in a fixed order from
  both privilege levels and printed on the completion path.
  Absolute counter samples are deliberately excluded from the
  checksum: they are timing values that differ run to run.
- `mir_strap.S`: S-mode trap entry (direct mode). Records
  `scause`/`stval`/`sepc`, bumps the S-mode trap counter,
  advances `sepc` by 4 (exact: `rdinstret`, like `rdtime` and
  `rdcycle`, has no compressed encoding), and `sret`.
- `mir_mtrap.S`: M-mode trap entry (direct mode). Records
  `mcause`/`mepc`, bumps the M-mode trap count, and parks the
  hart unless the entry is the expected `ecall` from S-mode
  (`mcause = 9`); on the expected entry it reopens the gate
  (`mcounteren = 0x7`, readback stored) and `mret`s with MPP=01
  into the phase-B S-mode payload.
- `build.sh`, `PROOF.md` (this file), `bench-logs/` with the build
  log and three raw QEMU run logs.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 17 checks held. On PASS the machine is
shut down through the virt test-device finisher (QEMU exits 0); on
FAIL the hart parks in a `wfi` loop without touching the finisher.

Build: direct `riscv64-unknown-elf-gcc` invocations matching the
repo Makefile pattern (`-Wall -Wextra -O2 -ffreestanding
-nostdlib -nostartfiles -no-pie -fno-pie -fno-pic
-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`), logged in
`bench-logs/build.log` via `build.sh`; the Makefile itself was not
modified.
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mcounteren-ir-gate.elf` under `timeout`, so a parked-hart
FAIL is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the experiment drops to S-mode via
  `mret` with `mstatus.MPP = 01`, returns to M-mode via `ecall`,
  and drops again via `mret`.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`),
  `riscv64-unknown-elf-gcc` 13.2.0, `-march=rv64imac_zicsr`.
- `medeleg = 0x4` (only the illegal-instruction trap delegated to
  S-mode; the S-mode `ecall` reaches M-mode).

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs exited 0 via the test-device finisher. The three logs are
byte-identical except for the absolute `rdcycle`/`rdinstret`
sample values (which are live counters); every verdict-relevant
line is identical across runs.

| step | operation | measured readback |
|---|---|---|
| boot | `csrr mcounteren` | 0x0 on all 3 runs |
| probe | `csrw mcounteren, 0x7`; readback | 0x7 on all 3 runs |
| gate | `csrw mcounteren, 0x3`; readback | 0x3 (CY|TM set, IR clear) on all 3 runs |
| setup | `medeleg` readback | 0x4 on all 3 runs |
| phase A | S-mode `rdinstret` at 0x80000286 with `mcounteren = 0x3` | trap, `scause = 0x2`, `stval = 0xc0202473`, `sepc = 0x80000286` (exactly the rdinstret site), `sepc+4 = 0x8000028a` (the resume label), S-mode trap count 1, dest register still the 0xdeadbeefdeadbeef sentinel |
| phase A | one S-mode `rdcycle` with CY set | no new trap (S-mode trap count still 1), strictly positive sample: 0xdc30106a78 on run 1 |
| M-mode | `ecall` from S-mode | `mcause = 0x9`, `mepc = 0x80000470` (exactly the recorded `ecall` site), M-mode trap count 1, `mcounteren` readback after reopen exactly 0x7 |
| phase B | two S-mode `rdinstret` with `mcounteren = 0x7` | no new trap (S-mode trap count still 1), samples strictly positive and strictly increasing: deltas +284055, +338760, +275295 across the three runs |

Checks: 17 (probe readback, gate readback, medeleg bit 2,
phase-A S-mode trap count == 1, scause == 2, sepc == fault site,
sepc+4 == resume label, sentinel intact, phase-A rdcycle trap
count still 1, rdcycle sample strictly positive, M-mode
mcause == 9, mepc == ecall site, M-mode trap count == 1,
mcounteren reopen readback == 0x7, phase-B S-mode trap count
still 1, first rdinstret sample strictly positive, second sample
strictly greater than the first). Mismatches: 0.
FNV-1a checksum over the seventeen verdict-relevant values:
0xdd2308106691e2e3, identical on all 3 runs.

Note: `stval = 0xc0202473` is QEMU's informational readback of the
faulting `rdinstret` encoding for the illegal-instruction trap; it
is printed as observed and is not part of the verdict or the
checksum.

Note on the phase-B deltas: in this QEMU build (no `-icount`) the
`instret` counter is derived from the host clock, so two
`rdinstret` reads report elapsed wall time rather than a retired
instruction count. A short spin loop (200,000 iterations)
separates the two reads so the strict increase holds regardless
of the counter's time base. The verdict asserts only strict
positivity and strict increase, which is exactly what the
mechanism under test (the IR gate being open) guarantees; the
absolute values are timing noise and differ across runs.

## Limits of verification

- The +4 `sepc` advance in the S-mode handler is exact only
  because `rdinstret` has no compressed encoding; the module
  asserts the result (`sepc+4` equals the labeled resume address)
  rather than assuming it.
- The gate is exercised for the IR bit only; TM gating was covered
  by `src/mcounteren-time-gate` and CY gating by
  `src/scounteren-trap`.
- QEMU-specific readbacks observed but not asserted: boot
  `mcounteren = 0x0` and the `stval` encoding above.
- No restore of `mcounteren` is performed: the machine halts on
  the completion path, and M-mode restore semantics are already
  covered by `src/mcounteren`.
- U-mode gating was not tested; the module drops M-mode to S-mode
  only.
- Emulator behavior, not silicon: every number above was measured
  under QEMU 8.2.2 `virt`, not on a physical hart.
