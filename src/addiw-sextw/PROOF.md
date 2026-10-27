<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: addiw with immediate 0 is a sign extension of the low word

## What was built

`src/addiw-sextw/`: a bare-metal RISC-V program that checks, with
register readback, that `addiw` with immediate 0 sign-extends the low
32 bits of its input, while `addi` with immediate 0 leaves all 64 bits
unchanged. Three files, sharing only `src/boot.S` and `src/uart.c` with
the other demos.

- `addiw_main.c`: UART bring-up and the test. For each of five inputs
  it executes `addiw` with 0 and `addi` with 0 through volatile asm
  and prints the input and both results. The inputs are
  0x0000000080000001, 0x000000007fffffff, 0x0000000180000001,
  0xffffffff80000001, and 0x00000000ffffffff.
- `PROOF.md` (this file), `bench-logs/` with the build log, the full
  disassembly, and the raw QEMU run logs.

Build (exact command, from the repo root):

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib \
  -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr \
  -mabi=lp64 -mcmodel=medany -Isrc -T link.ld \
  -o src/addiw-sextw/bench-logs/addiw-sextw.elf \
  src/boot.S src/uart.c src/addiw-sextw/addiw_main.c
```

Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel src/addiw-sextw/bench-logs/addiw-sextw.elf`

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- Image linked at 0x80000000 (`link.ld`).

## Ground truth (from `bench-logs/addiw-sextw.dis`)

- The loop's test is a single instruction at `0x80000222`:
  `sext.w a0, s0`. That is the assembler's name for exactly
  `addiw a0, s0, 0`, the instruction the C source requests through
  volatile asm. The compiler did not replace the test with a branch or
  a different operation.
- The `addi` side of the loop prints as `mv a0, s0` at `0x80000230`,
  the assembler's name for exactly `addi a0, s0, 0`: a move, because
  adding zero to all 64 bits changes nothing.
- The five inputs sit in `vals` at `0x800002b8`, read one per loop
  iteration; the loop ends when the pointer reaches the end of the array.

## Results (from `bench-logs/qemu-run1.log` and `bench-logs/qemu-run2.log`)

```
sextw in=0x80000001 addiw=0xffffffff80000001 addi=0x80000001
sextw in=0x7fffffff addiw=0x7fffffff addi=0x7fffffff
sextw in=0x180000001 addiw=0xffffffff80000001 addi=0x180000001
sextw in=0xffffffff80000001 addiw=0xffffffff80000001 addi=0xffffffff80000001
sextw in=0xffffffff addiw=0xffffffffffffffff addi=0xffffffff
done
```

(UART prints hex without leading zeros, so 0x80000001 here is the
input 0x0000000080000001, and 0x180000001 is 0x0000000180000001.)

## Checks

- 5 `addiw` results, each equal to the sign extension of the input's
  low 32 bits: 5 checks, 0 mismatches.
- 5 `addi` results, each equal to the input: shown, not counted as
  checks, because the claim under test is the `addiw` behavior.
- Two runs byte-identical.

## Honest limit

These are emulator results (QEMU 8.2.2 functional execution), not
silicon measurements. No pipeline, timing, or implementation behavior
is asserted.
