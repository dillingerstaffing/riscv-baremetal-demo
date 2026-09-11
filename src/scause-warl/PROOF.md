<!-- PROOF-HEADER
Checks: 23
Mismatches: 0
Checksum: 0x8cbf212d46b76670
Environment: QEMU 8.2.2
Verdict: PASS
-->
# Proof: scause WARL software-write check

In M-mode on QEMU, software writes of all-ones and zero to the
S-mode cause register `scause` both take effect: the readbacks
equal the written values, so all 64 bits of the register are
software-writable on this hart. A delegated S-mode `ecall` then
reports `scause` = 9, proving trap entry overwrites the register
with the real cause regardless of the last software write (the
last write before the trap was all-ones). This is the S-mode
counterpart of the shipped `src/mcause-warl` module, which
probed the M-mode cause register.

## What was built

`src/scause-warl/`: a bare-metal RISC-V program that measures
the legalized value of the `scause` CSR after all-ones and zero
writes, then forces a delegated S-mode trap and checks the
register holds the trap cause. Four files, sharing only
`src/boot.S` and `src/uart.c` with the other demos.

- `scw_trap.S`: M-mode trap entry recording mcause/mepc and
  bumping a trap counter in the `scw_regs` array via mscratch.
  The only expected M-mode trap is the phase-complete signal (a
  deliberate illegal instruction, mcause = 2); the handler
  jumps to `scw_finish` with MPP set to M-mode. Any other M-mode
  trap parks the hart, which the harness observes as the
  timeout exit status.
- `scw_strap.S`: S-mode trap entry recording scause, stval,
  sepc, and sstatus.SPP in the `st_regs` array via sscratch.
  The only expected S-mode trap is the delegated ecall (cause
  9); the handler advances sepc past the 4-byte ecall so the
  run continues. Any other S-mode cause parks the hart.
- `scw_main.c`: records the boot `scause`/`mideleg` baselines,
  installs direct-mode mtvec/stvec with mscratch/sscratch,
  then runs phase 1 (WARL probes from M-mode: `csrw scause,
  all-ones` must read back all-ones as a fixed point of a
  second identical write, `csrw scause, zero` must read back
  zero, no trap may fire from either write), phase 2
  (writes exactly medeleg bit 9 and requires the readback to
  take, writes all-ones to scause a final time, opens the
  address space with one PMP NAPOT entry, and mret's with
  MPP=01 into the S-mode payload), the S-mode payload
  (executes ecall at a labeled 4-byte site; requires exactly
  one S-mode trap with scause = 9, sepc exactly at the ecall
  site, sepc+4 at the resume label, sstatus.SPP = 1, stval = 0,
  and the M-mode trap count still 0; then issues the
  phase-complete illegal instruction), and `scw_finish`
  (requires M-mode trap count 1, scause still 9, restores
  mideleg to the boot value). A failed check prints `FAIL`
  and increments the mismatches counter; `RESULT: PASS` is
  printed only when every check held. The verdict-relevant
  values feed a 64-bit FNV-1a digest printed as the last data
  line, so the three bench runs can be compared for
  byte-identical output. On PASS the machine is shut down
  through the virt test-device finisher (QEMU exits 0); on
  FAIL the hart parks without touching the finisher.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make scause-warl.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel scause-warl.elf` (or `make run-scause-warl`), under
`timeout` so a parked-hart FAIL is observable as exit status
124.

Toolchain: Debian `riscv64-unknown-elf-gcc` 13.2.0,
`-march=rv64imac_zicsr`. QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: hart 0, single hart, M-mode at boot (QEMU boots the ELF
  straight into M-mode with `-bios none`), dropping to S-mode
  for the trap phase.
- No interrupt source is ever armed and mstatus.MIE stays
  clear, so the only traps that can ever fire are the
  deliberate S-mode ecall and the deliberate phase-complete
  illegal instruction.
- medeleg is written as exactly bit 9, not boot|bit 9: the
  boot mideleg (0x1444) already delegates illegal instructions
  (bit 2), and the phase-complete signal is a deliberate
  illegal instruction that must reach M-mode. mideleg is
  restored to the boot value before the verdict.

## Measured results (3 QEMU runs, byte-identical)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All
three runs printed byte-identical output, `checks: 23
mismatches: 0`, `checksum: 0x8cbf212d46b76670`, `RESULT:
PASS`, and QEMU exited 0 via the test-device finisher on every
run.

| phase | operation | measured readback |
|---|---|---|
| boot | `csrr scause`, `csrr mideleg` | scause=0x0, mideleg=0x1444 |
| setup | `csrr mtvec`, `csrr stvec` | mtvec=0x800001c8, stvec=0x8000023c (both direct mode, handler addresses) |
| 1: WARL | `csrw scause, 0xffffffffffffffff` | readback=0xffffffffffffffff (write took); rewrite of the readback reads back identical (fixed point) |
| 1: WARL | `csrw scause, 0x0` | readback=0x0 (no bits read-only) |
| 1: WARL | trap count across both writes | M-mode trap count = 0 (no trap fired from a CSR write) |
| 2: delegate | `csrw medeleg, 0x200` | readback=0x200 (bit 9 took) |
| 2: pre-drop | `csrw scause, 0xffffffffffffffff` | readback=0xffffffffffffffff (last software write before the trap) |
| S-mode trap | ecall at 0x80000364 | scause=0x9, stval=0x0, sepc=0x80000364 (exactly the ecall site), sepc+4=0x80000368 (the resume label), S-mode traps=1, sstatus.SPP=1, M-mode traps=0 |
| finish | M-mode state after phase-complete | M-mode traps=1 (only the phase-complete trap), scause=0x9 (the M-mode trap entry did not overwrite it), mideleg restored=0x1444 |

Writable vs read-only, grounded in the readbacks: every one of
the 64 bits, including the interrupt bit (bit 63), read back
the written value after an all-ones write, and the zero write
read back zero, so no bit of scause is read-only on this hart.
The S-mode trap then overwrote the all-ones value with 9, the
environment-call-from-S-mode cause, which is the direct
observation that trap entry replaces whatever software wrote
last. The delegated ecall never reached M-mode (M-mode trap
count stayed 0 through the S-mode phase), and the later M-mode
phase-complete trap left scause untouched at 9.

## What was verified, and what was not

Verified: on QEMU 8.2.2, an all-ones write to scause legalizes
to 0xffffffffffffffff and a zero write to 0x0, with no trap
fired by either write; medeleg bit 9 takes the write and
routes the S-mode ecall to the S-mode handler with scause = 9,
sepc exactly at the ecall site, SPP = 1, and the M-mode trap
count unchanged; the three runs were byte-identical (digest
0x8cbf212d46b76670).

Not verified: behavior on real silicon. These numbers come
from the QEMU 8.2.2 CSR model, not from hardware. Also not
verified: interrupt causes in scause (the interrupt bit was
set and read back only as a data bit, not in a
live-interrupt context), and the behavior of scause writes
from S-mode itself (all software writes here were issued from
M-mode, which can access the register).

## Limits

- The module assumes mideleg bit 9 is clear at boot (checked,
  and the run fails loudly if it is not) and writable
  (checked via readback).
- The boot mideleg value 0x1444 is this emulator's choice; the
  module writes exactly bit 9 for the delegation phase (so the
  phase-complete illegal instruction reaches M-mode) and
  restores the recorded boot value before the verdict rather
  than assuming zero.
- mtvec=0x800001c8, stvec=0x8000023c, and the ecall site
  0x80000364 are this exact binary's layout; they are
  published as measurements, not as claims about any other
  build.
