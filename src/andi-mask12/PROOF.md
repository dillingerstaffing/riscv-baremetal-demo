<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: a 12-bit mask does not fit in an andi immediate

## What was built

`src/andi-mask12/`: a bare-metal RISC-V program that checks, with
register readback, that `andi` with the largest positive immediate
(2047, or 0x7FF) keeps only the low 11 bits, while the two ways of
masking with 0xFFF agree with each other and differ from it exactly
where bit 11 is set. Three files, sharing only `src/boot.S` and
`src/uart.c` with the other demos.

- `andi_main.c`: UART bring-up and the test. For each of four inputs
  it executes `andi` with 2047, the shift pair `slli` 52 then `srli`
  52, and `and` against a constant 4095 loaded with `li`, and prints
  the input and all three results. The inputs are 0x12345abc, 0xfff,
  0x1000, and 0xffff.
- `PROOF.md` (this file), `bench-logs/` with the build log, the full
  disassembly, the raw QEMU run logs, the assembler rejection log,
  and the compiler output for a C mask of 0xFFF.

Build (exact command, from the repo root):

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib \
  -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr \
  -mabi=lp64 -mcmodel=medany -Isrc -T link.ld \
  -o src/andi-mask12/bench-logs/andi-mask12.elf \
  src/boot.S src/uart.c src/andi-mask12/andi_main.c
```

Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel src/andi-mask12/bench-logs/andi-mask12.elf`

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- Image linked at 0x80000000 (`link.ld`).

## Why 0xFFF cannot be an andi immediate

The I-type immediate is 12 bits, sign-extended, so its range is
-2048 to 2047. A mask written as an immediate can therefore reach
0x7FF at most on the positive side; 0xFFF (4095) is outside the
range, and so is 0x800 (2048). The GNU assembler refuses both
(`bench-logs/gas-reject.log`):

```
Error: illegal operands `andi t0,s0,0xfff'
Error: illegal operands `andi t1,s0,0x800'
```

The C compiler, given `x & 0xFFFUL` at -O2 (GCC 13.2.0, rv64imac),
does not use `andi` at all. It emits the shift pair
(`bench-logs/gcc-mask12.s`):

```
slli a0, a0, 52
srli a0, a0, 52
```

Shifting left 52 throws away everything above bit 11, and shifting
right 52 brings the low 12 bits back down, zero-filled. That is the
mask, built from two instructions because one immediate cannot hold it.

## Results (from `bench-logs/qemu-run1.log` and `qemu-run2.log`)

```
v=0x12345abc andi2047=0x2bc shiftfff=0xabc andfff=0xabc
v=0xfff andi2047=0x7ff shiftfff=0xfff andfff=0xfff
v=0x1000 andi2047=0x0 shiftfff=0x0 andfff=0x0
v=0xffff andi2047=0x7ff shiftfff=0xfff andfff=0xfff
done
```

## Checks

1. Input 0x12345abc: `andi` with 2047 gives 0x2bc, the low 11 bits,
   while both 0xFFF methods give 0xabc, the low 12 bits. PASS.
2. Inputs 0xfff and 0xffff, where bit 11 is set: `andi` with 2047
   gives 0x7ff, losing bit 11, while both 0xFFF methods give 0xfff.
   The difference is exactly bit 11. PASS.
3. Input 0x1000, where bit 12 is set but bits 0 to 11 are clear: all
   three methods give 0. The shift pair and the loaded constant
   agree with the `andi` column where the masks overlap. PASS.
4. The shift pair and `and` against a loaded 4095 agree on all four
   inputs. Two independent constructions of the 0xFFF mask give the
   same answers. PASS.
5. The assembler rejects `andi` with 0xfff and with 0x800, and the
   compiler's 0xFFF mask is the shift pair, not an `andi`. PASS.
6. Two runs produce identical logs. PASS.

## Verdict

6 checks, 0 mismatches, QEMU 8.2.2. An `andi` immediate holds at
most 0x7FF as a positive mask; a 12-bit mask needs the shift pair
or a loaded constant instead.

## Honest limit

These are emulator results, not silicon. No claim depends on how any
core implements immediate AND; QEMU serves as a functional executor
only. The assembler rejection is a property of the GNU assembler in
the toolchain used, not of the instruction encoding.
