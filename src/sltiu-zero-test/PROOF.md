<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: sltiu against 1 is a branchless zero test

## What was built

`src/sltiu-zero-test/`: a bare-metal RISC-V program that checks, with
register readback, that `sltiu` against the immediate 1 returns 1 exactly
when its input is zero. Three files, sharing only `src/boot.S` and
`src/uart.c` with the other demos.

- `sltiu_main.c`: UART bring-up and the test. For each of five inputs it
  executes `sltiu` against 1 through volatile asm and prints the input
  and the result. The inputs are 0, 1, 2, 5, and 0xffffffffffffffff.
- `PROOF.md` (this file), `bench-logs/` with the build log, the full
  disassembly, and the raw QEMU run logs.

Build (exact command, from the repo root):

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib \
  -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr \
  -mabi=lp64 -mcmodel=medany -Isrc -T link.ld \
  -o src/sltiu-zero-test/bench-logs/sltiu-zero-test.elf \
  src/boot.S src/uart.c src/sltiu-zero-test/sltiu_main.c
```

Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel src/sltiu-zero-test/bench-logs/sltiu-zero-test.elf`

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- Image linked at 0x80000000 (`link.ld`).

## Ground truth (from `bench-logs/sltiu-zero-test.dis`)

- The loop's test is a single instruction at `0x80000218`:
  `seqz a0, s0`. That is the assembler's name for exactly
  `sltiu a0, s0, 1`, the instruction the C source requests through
  volatile asm. The compiler did not replace the test with a branch or
  a different comparison.
- The five inputs sit in `vals` at `0x800002a0`, read one per loop
  iteration; the loop ends when the pointer reaches the end of the array.

## Results (from `bench-logs/qemu-run1.log` and `qemu-run2.log`)

```
sltiu(v, 1) v=0x0 -> 1
sltiu(v, 1) v=0x1 -> 0
sltiu(v, 1) v=0x2 -> 0
sltiu(v, 1) v=0x5 -> 0
sltiu(v, 1) v=0xffffffffffffffff -> 0
done
```

## Checks

1. Input 0 returns 1. The only unsigned value below 1 is 0, so the
   zero test fires exactly once. PASS.
2. Inputs 1, 2, and 5 return 0. Nonzero values below no threshold other
   than 1 itself do not fire. PASS.
3. Input 0xffffffffffffffff returns 0. Under the unsigned comparison
   this is the largest value, not negative one, so it does not fire;
   the signed twin `slti` would have returned 1 here. PASS.
4. The disassembly test is `seqz a0, s0` at `0x80000218`, i.e. the
   requested `sltiu` against 1, not a rewritten test. PASS.
5. Two runs produce identical logs. PASS.

## Verdict

5 checks, 0 mismatches, QEMU 8.2.2. `sltiu` against 1 is a zero test:
it returns 1 exactly when its input is zero, because zero is the only
unsigned value below 1.

## Honest limit

These are emulator results, not silicon. No claim depends on how any
core implements set-less-than-immediate; QEMU serves as a functional
executor only.
