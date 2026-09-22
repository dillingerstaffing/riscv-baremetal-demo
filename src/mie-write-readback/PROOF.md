<!-- PROOF-HEADER
Checks: 10
Mismatches: 0
Checksum: 0xf75a313eb8844187
Environment: QEMU 8.2.2
Verdict: PASS
-->
# Proof: mie WARL all-ones write/readback probe

## What was built

`src/mie-write-readback/`: a bare-metal RISC-V program that measures
the legalized value of the mie CSR after an all-ones write, in
M-mode on the QEMU `virt` board. Three files, sharing only
`src/boot.S` and `src/uart.c` with the other demos. Exactly one
mechanism is under test: which bits of mie survive a write of all
ones, and which legalize away.

- `mwr_trap.S`: M-mode trap entry recording mcause/mepc/mtval and
  bumping a trap counter in the `mwr_regs` array via mscratch.
  mstatus.MIE stays clear for the whole run, so the handler is
  expected never to fire; it advances mepc past the trapping
  instruction so a surprise trap cannot loop silently.
- `mwr_main.c`: records the boot baselines (mie, mstatus, mideleg),
  verifies mstatus.MIE is clear at boot, installs direct-mode mtvec
  and mscratch, then runs phase 1 (`csrw mie, all-ones`, publish
  the readback; `csrw mie, zero`, must read back 0), phase 2 (per-bit
  probe of bits 0..11: each write of `(1UL << b)` must read back
  exactly `(1UL << b)` or exactly 0, no stray bits), phase 3
  (all-ones again: must read back equal to the phase-1 readback,
  and its low 12 bits must equal the OR of the per-bit sticky
  results), and phase 4 (restore mie to the exact boot value,
  verify byte-identical readback; verify mstatus.MIE still clear).
  A failed check prints `FAIL` and increments the mismatches
  counter; `RESULT: PASS` is printed only when every check held.
  The verdict-relevant values feed a 64-bit FNV-1a digest printed
  as the last data line, so the three bench runs can be compared
  for byte-identical output. On PASS the machine is shut down
  through the virt test-device finisher (QEMU exits 0); on FAIL the
  hart parks without touching the finisher.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make mie-write-readback.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mie-write-readback.elf` (or `make run-mie-write-readback`),
under `timeout` so a parked-hart FAIL is observable as exit status
124.

Toolchain: Debian `riscv64-unknown-elf-gcc` 13.2.0,
`-march=rv64imac_zicsr`. QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: hart 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- mie, mstatus.MIE read back clear at boot, and mstatus.MIE reads
  clear at the end, so the asserted enable bits cannot be taken as
  interrupts; the trap count must be 0. This is distinct from the
  done `mie-mtie-gate` module, which proved interrupt gating
  behavior, not register write/readback legalization.

## Measured results (3 QEMU runs, byte-identical)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs printed byte-identical output, `checks: 10 mismatches: 0`,
`digest: 0xf75a313eb8844187`, `RESULT: PASS`, and QEMU exited 0
via the test-device finisher on every run.

| phase | operation | measured readback |
|---|---|---|
| boot | `csrr mie`, `csrr mstatus`, `csrr mideleg` | mie=0x0, mstatus=0xa00000000, mideleg=0x1444 |
| 1: ones | `csrw mie, 0xffffffffffffffff` | mie=0xfffffffffffffeee |
| 1: zero | `csrw mie, 0x0` | mie=0x0 |
| 2: bits 0..11 | `csrw mie, 1<<b`, read back per bit | bit 0: 0x0, bit 1: 0x2, bit 2: 0x4, bit 3: 0x8, bit 4: 0x0, bit 5: 0x20, bit 6: 0x40, bit 7: 0x80, bit 8: 0x0, bit 9: 0x200, bit 10: 0x400, bit 11: 0x800 |
| 2: union | OR of per-bit sticky results | 0xeee |
| 3: ones | `csrw mie, 0xffffffffffffffff` | mie=0xfffffffffffffeee (== phase 1; low 12 bits == 0xeee union) |
| 4: restore | `csrw mie, boot mie` | mie=0x0 (byte-identical), mstatus.MIE still clear |
| traps | handler installed, mstatus.MIE=0 throughout | trap count = 0 on every run |

Sticky vs legalized, grounded in the readbacks: of the standard
interrupt-enable positions (bits 0..11), bits 1, 2, 3, 5, 6, 7, 9,
10, 11 stick and bits 0, 4, 8 legalize away. Bits 12..63 all read
back 1 on the all-ones write (the phase-1 readback is
0xfffffffffffffeee, i.e. everything except bits 0 and 4). The
all-ones readback in phase 3 matched phase 1 exactly, and its low
12 bits matched the OR of the per-bit results, so the bits that
stick one at a time are exactly the low bits that stick all at
once. A zero write reads back exactly 0, so every provided bit is
software-writable. mie was restored to the exact boot value (0x0)
at the end.

## What was verified, and what was not

Verified: on QEMU 8.2.2, an all-ones write to mie in M-mode reads
back 0xfffffffffffffeee (bits 1,2,3,5,6,7,9,10,11 and all of bits
12..63 set; bits 0, 4, 8 legalized away); a zero write reads back
0x0; a second all-ones write reads back identically; bits 0..11
individually probe to either the written bit or 0 with no stray
bits; the boot mie value is restored exactly; 0 traps; mstatus.MIE
clear at boot and at end; the three runs were byte-identical
(digest 0xf75a313eb8844187).

Not verified: behavior on real silicon. These numbers come from the
QEMU 8.2.2 CSR model, not from hardware. Also not verified: bits
12..63 individually (observed set on the all-ones readback but not
per-bit probed), interrupt delivery behavior (deliberately out of
scope: the done `mie-mtie-gate` module covers gating), and any
S-mode view of the register (S-mode cannot write mie at all).

## Limits

- The module assumes mstatus.MIE is clear at boot (checked, and the
  run fails loudly if it is not); writing enable bits is safe only
  because no trap can be taken without MIE.
- The boot mie value 0x0 and mstatus 0xa00000000 are this
  emulator's choice; the module restores the recorded boot value
  rather than assuming zero.
