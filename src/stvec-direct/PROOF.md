<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: stvec direct-mode write/readback + delegated S-mode trap delivery

## What was built

`src/stvec-direct/`: a bare-metal RISC-V program that checks the
supervisor trap-vector base register (`stvec`) in direct mode and
trap delivery to the `stvec` base for a delegated S-mode environment
call, on the QEMU `virt` board. Two files, sharing only `src/boot.S`
and `src/uart.c` with the other demos. Exactly one mechanism is under
test: writing `stvec` with MODE=00 and taking a `medeleg`-delegated
trap to the `stvec` base.

- `svd_trap.S`: S-mode direct-mode trap entry, installed as the
  `stvec` base. Saves the registers it clobbers (t0 via the `sscratch`
  swap), records `scause`/`sepc`/`stval` into C-visible globals, bumps
  a trap counter, prints the trap record with the measured numbers,
  and prints `RESULT: PASS` only if every check holds: exactly 1
  trap, `scause == 9` (environment call from S-mode), `sepc ==` the
  captured ecall address, `stval == 0`. On PASS it shuts the machine
  down via the virt test-device finisher (QEMU exits 0); on FAIL it
  parks the hart in a `wfi` loop. There is no trap return: one trap
  per run is the whole experiment.
- `svd_main.c`: M-mode setup. Records the boot-time `stvec`, writes
  `stvec` = handler base with the low two bits clear (MODE=00), reads
  it back and checks the mode bits read back clear and the base
  matches the written address exactly; sets `medeleg` bit 9
  (supervisor environment call) and verifies the bit reads back set;
  opens the address space with one PMP NAPOT R/W/X entry (S-mode is
  default-deny with no PMP entry programmed); then `mret`s with
  `mstatus.MPP=01` into an S-mode payload that is a single `ecall`.
  The ecall address is captured with `la t0, 1f` (a numeric asm local
  label; the assembler resolves it exactly, unlike a C
  labels-as-values address, which gcc misplaces at -O2), and the
  payload ecall is wrapped in `.option norvc` so it is exactly 4
  bytes and the captured address is exact.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make stvec-direct.elf` (added to `all` and `clean` in the
Makefile). Run: `qemu-system-riscv64 -machine virt -nographic -bios
none -kernel stvec-direct.elf` (or `make run-stvec-direct`, with
`QEMU` set to the repo's QEMU 8.2.2 build).

## Configuration under test

- Hart: mhartid = 0, single hart, booted straight into M-mode with
  `-bios none`.
- QEMU 8.2.2 (`~/workspace/qemu/usr/bin/qemu-system-riscv64`).
- `stvec` handler base `svd_trap_entry` is `.align 2` (4-byte
  aligned), so the MODE=00 write carries no stray mode bits.

## Measured results

Three runs under QEMU 8.2.2, byte-identical output, all
`RESULT: PASS`:

| Quantity            | Run 1       | Run 2       | Run 3       |
|---------------------|-------------|-------------|-------------|
| `stvec` at boot     | 0x0         | 0x0         | 0x0         |
| `stvec` write       | 0x800001c8  | 0x800001c8  | 0x800001c8  |
| `stvec` readback    | 0x800001c8  | 0x800001c8  | 0x800001c8  |
| mode bits readback  | 0 (clear)   | 0 (clear)   | 0 (clear)   |
| `medeleg` at boot   | 0x0         | 0x0         | 0x0         |
| `medeleg` after csrs bit 9 | 0x200 | 0x200    | 0x200       |
| `scause`            | 0x9         | 0x9         | 0x9         |
| `sepc`              | 0x8000058e  | 0x8000058e  | 0x8000058e  |
| `stval`             | 0x0         | 0x0         | 0x0         |
| captured ecall addr | 0x8000058e  | 0x8000058e  | 0x8000058e  |
| traps this run      | 1           | 1           | 1           |

Checks enforced in code (any failure parks the hart instead of
exiting): the handler base is 4-byte aligned; the `stvec` readback
has its mode bits clear and its base equal to the written address;
`medeleg` bit 9 reads back set; the handler's `scause == 9`,
`sepc == ecall_addr` exactly, `stval == 0`, and the trap count is 1.

Disassembly check (via objdump): `la t0, 1f` resolved to exactly
`0x8000058e`, the address of the `ecall` (`00000073`, 4 bytes, no
compressed form), so the `sepc == ecall_addr` equality is a real
address match, not a coincidence of truncation.

Raw logs: `bench-logs/build.log`, `bench-logs/run1.log`,
`bench-logs/run2.log`, `bench-logs/run3.log`.

## Sequence and controls

1. The `stvec` write/readback pair is published before any trap, so
   the register behavior is observed independently of trap
   delivery.
2. The `medeleg` readback (0x200) proves bit 9 was accepted; without
   it the probe ecall would have trapped to M-mode through `mtvec`
   (zero at boot) instead of to S-mode through `stvec`.
3. The payload is exactly one `ecall` at the captured address, so
   `sepc` has only one possible correct value.

## Honest limits

- These are emulator measurements, not silicon: the cause encoding
  (`scause == 9` for an S-mode environment call) is spec-mandated,
  but the absolute addresses (`0x800001c8`, `0x8000058e`) are
  specific to this build and the QEMU `virt` machine.
- Only MODE=00 (direct) is exercised; vectored `stvec` (MODE=01) is
  not part of this module.
- Only one delegated trap source (the S-mode environment call) is
  exercised; other `medeleg` bits and interrupt delegation are
  covered by sibling modules.
