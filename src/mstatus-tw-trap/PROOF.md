<!-- PROOF-HEADER
Checks: 6
Mismatches: 0
Checksum: 0xc32b722c5100e880
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mstatus.TW makes an S-mode WFI trap as illegal instruction

## What was built

`src/mstatus-tw-trap/`: a bare-metal RISC-V program that verifies,
on the QEMU `virt` board, that the TW bit (mstatus bit 21) turns an
S-mode `wfi` into an illegal-instruction exception. M-mode sets
`mstatus.TW = 1` (verified by readback), delegates the
illegal-instruction trap to S-mode, and drops to S-mode; the S-mode
payload executes `wfi` at a labeled 4-byte site and must take
exactly one S-mode trap with `scause = 2` and `sepc` exactly at the
`wfi` site. The module shares only `src/boot.S` and `src/uart.c`
with the other demos.

- `tw_main.c`: M-mode setup (boot `mstatus` readback, TW write of
  bit 21 with readback, `mtvec`/`stvec`/`sscratch` install,
  `medeleg` bit 2 so the illegal-instruction trap is delivered to
  S-mode, a whole-address-space PMP NAPOT entry, then `mret` with
  MPP=01 into the S-mode payload). The S-mode payload runs one
  labeled `wfi` with `mstatus.TW = 1`, expecting exactly one
  S-mode trap with `scause = 2`, `sepc` exactly at the wfi site,
  and the handler's +4 advance landing on the labeled resume
  address; the payload executes no further `wfi` after the resume,
  so no second trap is possible. The fault site uses in-asm
  numeric local labels (`la t, 1f` with `1:` in the asm) so the
  assembler resolves the exact address; the C `&&label` construct
  is never used for trap-resume addresses. A 64-bit FNV-1a
  checksum is fed the six verdict-relevant values in a fixed order
  from both privilege levels and printed on the completion path.
- `tw_strap.S`: S-mode trap entry (direct mode). Records
  `scause`/`stval`/`sepc`, bumps the trap counter, advances `sepc`
  by 4 (exact: `wfi` has no compressed encoding), and `sret`.
- `tw_mtrap.S`: M-mode trap entry (direct mode). A correct run
  takes no M-mode trap at all (no M-mode ecall is issued), so this
  handler parks the hart on any entry.
- `build.sh`, `PROOF.md` (this file), `bench-logs/` with the build
  log and three raw QEMU run logs.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 6 checks held. On PASS the machine is
shut down through the virt test-device finisher (QEMU exits 0); on
FAIL the hart parks in a `wfi` loop, and with TW still set each
`wfi` raises the delegated illegal-instruction trap while the
handler resumes past it, so the hart spins in a trap loop until
the bench harness timeout (exit status 124).

Build: direct `riscv64-unknown-elf-gcc` invocations matching the
repo Makefile pattern (`-Wall -Wextra -O2 -ffreestanding
-nostdlib -nostartfiles -no-pie -fno-pie -fno-pic
-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`), logged in
`bench-logs/build.log` via `build.sh`; the Makefile itself was not
modified.
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mstatus-tw-trap.elf` under `timeout`, so a parked-hart
FAIL is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the experiment drops to S-mode via
  `mret` with `mstatus.MPP = 01` (TW, bit 21, is outside the masked
  MPP bits, so it stays set across the drop).
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`),
  `riscv64-unknown-elf-gcc` 13.2.0, `-march=rv64imac_zicsr`.
- `medeleg = 0x4` (only the illegal-instruction trap delegated to
  S-mode).

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs exited 0 via the test-device finisher, and the three logs are
byte-identical (verified with `cmp`).

| step | operation | measured readback |
|---|---|---|
| boot | `csrr mstatus` | 0xa00000000 on all 3 runs |
| TW write | `csrw mstatus, mstatus\|(1<<21)`; readback | 0xa00200000 (bit 21 set) on all 3 runs |
| setup | `medeleg` readback | 0x4 on all 3 runs |
| payload | S-mode `wfi` at 0x8000026e with `mstatus.TW = 1` | trap, `scause = 0x2`, `stval = 0x10500073`, `sepc = 0x8000026e` (exactly the wfi site), `sepc+4 = 0x80000272` (exactly the resume label), trap count 1 |

Checks: 6 (TW readback has bit 21 set, medeleg bit 2 set, trap
count == 1, scause == 2, sepc == wfi site, sepc+4 == resume
label). Mismatches: 0. FNV-1a checksum over the six
verdict-relevant values: 0xc32b722c5100e880, identical on all 3
runs.

Note: `stval = 0x10500073` is QEMU's informational readback of the
faulting `wfi` encoding for the illegal-instruction trap; it is
printed as observed and is not part of the verdict or the
checksum.

## Limits of verification

- The control case (TW clear, `wfi` in S-mode) is intentionally
  omitted: an untrapped S-mode `wfi` would block indefinitely with
  no interrupt source armed, so it could not complete a run. The
  shipped verification is the TW=1 trap behavior together with the
  `mstatus.TW` readback, exactly what was measured.
- The +4 `sepc` advance in the S-mode handler is exact only
  because `wfi` has no compressed encoding; the module asserts the
  result (`sepc+4` equals the labeled resume address) rather than
  assuming it.
- QEMU-specific readbacks observed but not asserted: boot
  `mstatus = 0xa00000000` and the `stval` encoding above.
- No restore of `mstatus.TW` is performed: the machine halts on
  the completion path.
- U-mode behavior was not tested; the module drops M-mode to
  S-mode only.
- Emulator behavior, not silicon: every number above was measured
  under QEMU 8.2.2 `virt`, not on a physical hart.
