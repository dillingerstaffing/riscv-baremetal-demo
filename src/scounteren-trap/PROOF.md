<!-- PROOF-HEADER
Checks: 12
Mismatches: 0
Checksum: 0xefbc32d301fc1227
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mcounteren gates S-mode rdcycle with an illegal-instruction trap

## What was built

`src/scounteren-trap/`: a bare-metal RISC-V program that verifies, on
the QEMU `virt` board, the exact gating behavior of the `mcounteren`
CSR for S-mode: with `mcounteren = 0` an S-mode `rdcycle` raises an
illegal-instruction exception instead of returning a count, and with
the CY bit set the same S-mode `rdcycle` succeeds and returns an
advancing count. M-mode reads are never gated, so the experiment
requires a real privilege drop to observe the gate. The module shares
only `src/boot.S` and `src/uart.c` with the other demos.

- `st_main.c`: M-mode setup (boot `mcounteren` readback, 0x7
  writability probe, gate write of 0, `mtvec`/`stvec`/`sscratch`
  install, `medeleg` bit 2 so the illegal-instruction trap is
  delivered to S-mode while the S-mode ecall stays in M-mode, a
  whole-address-space PMP NAPOT entry, then `mret` with MPP=01 into
  the S-mode payload). The S-mode payload runs phase A (labeled
  `rdcycle` with `mcounteren = 0`, expecting exactly one S-mode
  trap with `scause = 2` and `sepc` exactly at the rdcycle site),
  then an `ecall` into the M-mode helper, then phase B (labeled
  `rdcycle` with CY set, expecting no new trap and two strictly
  increasing samples). The fault sites use in-asm numeric local
  labels (`la t, 1f` with `1:` in the asm) so the assembler
  resolves the exact address; the C `&&label` construct is never
  used for trap-resume addresses. A 64-bit FNV-1a checksum is fed
  the twelve verdict-relevant values in a fixed order from both
  privilege levels and printed on the completion path.
- `st_trap.S`: S-mode trap entry (direct mode). Records
  `scause`/`stval`/`sepc`, bumps the trap counter, advances `sepc`
  by 4 (exact: `rdcycle` has no compressed encoding), and `sret`.
- `mt_trap.S`: M-mode trap entry (direct mode). Services only the
  S-mode ecall (`mcause` 9): sets the `mcounteren` CY bit, records
  the resulting value and the ecall count, skips the 4-byte ecall,
  and `mret`s back to S-mode. Any other `mcause` parks the hart.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 12 checks held. On PASS the machine is
shut down through the virt test-device finisher (QEMU exits 0); on
FAIL the hart parks in a `wfi` loop without touching the finisher.

Build: direct `riscv64-unknown-elf-gcc` invocations matching the
repo Makefile pattern (`-Wall -Wextra -O2 -ffreestanding
-nostdlib -nostartfiles -no-pie -fno-pie -fno-pic
-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`), logged in
`bench-logs/build.log`; the Makefile itself was not modified.
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel scounteren-trap.elf` under `timeout` so a parked-hart FAIL
is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the experiment drops to S-mode via
  `mret` with `mstatus.MPP = 01`.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`), xpack
  `riscv64-unknown-elf-gcc` 15.2.0, `-march=rv64imac_zicsr`.
- `medeleg = 0x4` (only the illegal-instruction trap delegated to
  S-mode); S-mode `ecall` (cause 9) still traps to M-mode.

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
| gate | `csrw mcounteren, 0`; readback | 0x0 on all 3 runs |
| setup | `medeleg` readback | 0x4 on all 3 runs |
| phase A | S-mode `rdcycle` at 0x8000031e with `mcounteren = 0` | trap, `scause = 0x2`, `stval = 0xc0002473`, `sepc = 0x8000031e` (exactly the rdcycle site), `sepc+4 = 0x80000322` (the resume label), trap count 1, dest register still the 0xdeadbeefdeadbeef sentinel |
| phase B | S-mode `ecall` | M-mode helper ran once, `mcounteren` after = 0x1 (CY set) |
| phase B | S-mode `rdcycle` with CY set | no new trap (S-mode trap count still 1), samples strictly increasing: deltas +25755, +7800, +6630 across the three runs |

Checks: 12 (probe readback, zero readback, medeleg bit 2,
scause == 2, sepc == fault site, sepc+4 == resume label, sentinel
intact, phase-A trap count == 1, M-mode ecall count == 1, CY set,
phase-B trap count still 1, samples advancing). Mismatches: 0.
FNV-1a checksum over the twelve verdict-relevant values:
0xefbc32d301fc1227, identical on all 3 runs.

Note: `stval = 0xc0002473` is QEMU's informational readback of the
faulting `rdcycle` encoding for the illegal-instruction trap; it is
printed as observed and is not part of the verdict or the
checksum.

## Limits of verification

- The +4 `sepc` advance in the S-mode handler is exact only
  because `rdcycle` has no compressed encoding; the module asserts
  the result (`sepc+4` equals the labeled resume address) rather
  than assuming it.
- The gate is exercised for the CY bit only; TM and IR gating
  (`rdtime`, `rdinstret`) were not tested here.
- QEMU-specific readbacks observed but not asserted: boot
  `mcounteren = 0x0` and the `stval` encoding above.
- No restore of `mcounteren` is performed: the machine halts on
  the completion path, and M-mode restore semantics are already
  covered by `src/mcounteren`.
- U-mode gating was not tested; the module drops M-mode to S-mode
  only.
