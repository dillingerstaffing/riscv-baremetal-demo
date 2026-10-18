<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: blt compares signed, bltu compares unsigned

## What was built

`src/blt-bltu-addr/`: a bare-metal RISC-V program that executes the signed
branch (`blt`) and the unsigned branch (`bltu`) on the same register pairs
and records which branch is taken. Three files, sharing only `src/boot.S`
and `src/uart.c` with the other demos.

- `bba_main.c`: UART bring-up and six checks. Two addresses are held in
  registers: `hi = 0x8000000000000000` (bit 63 set, negative as int64_t)
  and `lo = 0x0000000000001000` (positive either way), plus a high-half
  address `kern = 0xfffffffffffff000` (-4096 as int64_t). Each check issues
  one real `blt` or `bltu` via inline assembly and records taken/not-taken.
- `bba_compare.c`: compiled to assembly only, never run. Two one-branch
  range checks, one on `long` and one on `unsigned long`, showing which
  compare instruction the compiler picks for each signedness.
- `PROOF.md` (this file), `bench-logs/` with the build log, the full
  disassembly, the compiler listing, and the raw QEMU run log.

Build (exact command, from the repo root):

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib \
  -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr \
  -mabi=lp64 -mcmodel=medany -Isrc -T link.ld -o blt-bltu-addr.elf \
  src/boot.S src/uart.c src/blt-bltu-addr/bba_main.c
```

Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel blt-bltu-addr.elf`

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- Image linked at 0x80000000 (`link.ld`).
- Toolchain: riscv64-unknown-elf-gcc 13.2.0 (Ubuntu rv64 packages).
- QEMU: 8.2.2 (`~/workspace/qemu`).

## Ground truth (from `bench-logs/blt-bltu-addr.dis`)

The inline assembly survived optimization as real branch instructions:

- `blt s0,s1,...` at `0x80000292` and `0x800002ae` (the two signed checks)
- `bltu s0,s1,...` at `0x800002ca` and `0x800002e6` (the two unsigned checks)
- `blt s0,s1,...` at `0x80000304` and `bltu s0,s1,...` at `0x80000320`
  (the high-half address pair)

Compiler listing (from `bench-logs/bba_compare.s`, `-O2`):

- `below_signed` (`long` operands): emits `slt a0,a0,a1` (signed set-less-than)
- `below_unsigned` (`unsigned long` operands): emits `sltu a0,a0,a1` (unsigned)

The signedness of the C type selects the signed vs unsigned instruction;
the branch pair below shows what that choice does to the address map.

## Result (from `bench-logs/qemu-run.log`)

```
hi = 0x8000000000000000
lo = 0x1000
kern = 0xfffffffffffff000
blt (hi, lo) : got 1, want 1  ok
blt (lo, hi) : got 0, want 0  ok
bltu(hi, lo) : got 0, want 0  ok
bltu(lo, hi) : got 1, want 1  ok
blt (kern, lo): got 1, want 1  ok
bltu(kern, lo): got 0, want 0  ok
mismatches: 0
PASS
```

Reading it: `blt` treats `0x8000000000000000` as the most negative int64
and takes the branch against `0x1000`; `bltu` treats the same register as
the largest address and does not. The applied pair: a signed range check
`kern < lo` says the high-half address `0xfffffffffffff000` is *below*
`0x1000` (taken), while the unsigned check says it is above (not taken).
Same two registers, opposite verdicts; the only difference is the
instruction's signedness.

Checks: 6. Mismatches: 0.
