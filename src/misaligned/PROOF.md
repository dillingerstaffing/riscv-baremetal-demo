<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: misaligned load/store experiment

## What was built

`src/misaligned/`: a bare-metal RISC-V program that installs an M-mode
trap handler recording mcause/mepc/mtval on any trap, then issues a
misaligned `lw` and a misaligned `sw`, each at a fixed unaligned
address. For each access it reports whether a trap fired. On a trap it
prints the exact trap register values; on transparent completion it
prints the observed loaded/stored values and checks them against
ground truth. Four files, about 250 lines total, sharing only
`src/boot.S` and `src/uart.c` with the other demos.

- `mis_main.c`: UART bring-up, trap vector installation, aligned-access
  control, the misaligned lw test, the misaligned sw test, and the
  PASS/FAIL verdict. Each test is a single inline-asm block with an
  exact instruction layout (see below) so the faulting instruction's
  address is known; the program checks the trap's mepc against it.
- `mis_trap.S`: minimal M-mode trap entry. mscratch points at the
  8-word `mis_save` array; on entry it swaps t0, records
  mcause/mepc/mtval, loads mepc from the resume address the test stored,
  flags the trap seen, restores t0/t1, and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make mal.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel mal.elf`
(or `make run-mal`).

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- Scratch buffer: 64 bytes at `0x80000c00` (16-byte aligned).
- lw test address: `0x80000c01` (base+1, not 4-byte aligned).
- sw test address: `0x80000c02` (base+2, not 4-byte aligned), value
  `0x12345678`.

## Exact-layout construction

Each test's asm block is wrapped in `.option push` / `.option norvc` /
`.option pop` so every instruction in the trap window is the 4-byte
form. (Without this, the assembler folds `addi t0, reg, 0` into the
2-byte c.mv; this was caught by inspecting the disassembly, not by
guessing.) The disassembly confirms the layout for both tests:

- lw: auipc (A), addi (A+4), lw at A+8, resume label at A+16.
  Observed: A=0x8000043a, lw at 0x80000442, resume 0x80000446.
- sw: auipc (A), addi (A+4), addi (A+8), sw at A+12, resume at A+16.
  Observed: A=0x800004fc, sw at 0x80000508, resume 0x8000050e.

The program checks `mepc == resume - 8` (lw) and `mepc == resume - 4`
(sw) if a trap fires, so a layout mistake would show up as a FAIL,
not as a silent wrong number.

## Control

Before the tests, the scratch buffer is written and read back with
aligned word accesses (`0xA1B2C3D4 + i`): proves the address is good
RAM, so whatever the misaligned accesses do comes from the access
itself and not from a bad address.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

| test | trap fired? | mcause | mepc | mtval | observed values |
|---|---|---|---|---|---|
| misaligned lw @ 0x80000c01 | no | (none) | (none) | (none) | loaded 0xffffffffd5a1b2c3, matches sign-extended byte-wise reference 0xd5a1b2c3 |
| misaligned sw @ 0x80000c02 | no | (none) | (none) | (none) | bytes read back 78 56 34 12, matches stored 0x12345678; neighboring byte at base+1 untouched (0xC3) |

Identical across all three runs (the only log difference is the pid in
the timeout kill line). Every program-internal check passed; all three
runs print `RESULT: PASS`.

What each value means:

- No trap fired for either access: the seen flag stayed 0, and
  mcause/mepc/mtval read back 0 because the handler never ran. This
  is the measured ground truth on this machine: QEMU's `virt` board
  completes misaligned word loads and stores transparently.
- The lw loaded value `0xffffffffd5a1b2c3` is the sign-extended
  little-endian word at base+1: bytes at base+1..base+4 are
  C3 B2 A1 D5 (from the aligned control fill 0xA1B2C3D4 at base+0 and
  0xA1B2C3D5 at base+4), assembled little-endian as 0xd5a1b2c3, and
  `lw` sign-extends bit 31. The program compares against exactly that
  sign-extended reconstruction, computed byte by byte at run time.
- The sw round-trips exactly: the four affected bytes read back as
  78 56 34 12, and the byte just outside the stored range (base+1,
  still 0xC3 from the control fill) is untouched, proving the store
  touched exactly the intended four bytes.

## One defect found and fixed during development

The transparent lw check initially compared the loaded value against
the unsigned 32-bit reference `0xd5a1b2c3` and failed, because `lw`
sign-extends. The value in the register was correct
(`0xffffffffd5a1b2c3`); the check was wrong. Fixed by comparing
against `(long)(int)expected`, i.e. the sign-extended reference.
Caught by the program's own check printing FAIL on the first run.

## Limits of verification (read before citing numbers)

- This measures the behavior of QEMU 8.2.2's `virt` machine, which
  emulates unaligned accesses in software. It is emulator behavior,
  not silicon: real RISC-V implementations may trap misaligned
  accesses (mcause 4 for loads, 6 for stores) or handle them in
  hardware, depending on the core.
- Only one misaligned offset each for lw and sw, only hart 0, only
  M-mode, only word accesses. Halfword, doubleword, and AMO
  misalignment are not tested; the module is deliberately that small.
- Addresses are specific to this binary's layout; the invariant that
  transfers is the mepc/resume relationship, re-checked by the program
  on every run.

## Reproduction

```
make mal.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mal.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; QEMU is terminated by `timeout` afterwards
because the bare-metal image never exits QEMU on its own).
