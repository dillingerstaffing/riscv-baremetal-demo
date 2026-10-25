<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: jalr clears bit 0 of its jump target

## What was built

`src/jalr-clears-bit0/`: a bare-metal RISC-V program that checks, against
the disassembly and the ISA contract, what jalr does when handed an odd
jump target. Two files, sharing only `src/boot.S` and `src/uart.c` with
the other demos.

- `jalr_main.c`: UART bring-up and four checks. Takes the address of a
  label, adds 1 to make it odd, and jumps to the odd address with
  `jalr t2, 0(odd)`. The label block stores 1 into `landed` and returns
  through t2. The checks: (1) the odd address really is odd, (2) the odd
  address with bit 0 cleared equals the label address, (3) the label
  address is even, (4) the label block ran (`landed == 1`).
- `PROOF.md` (this file), `bench-logs/` with the build log, the full
  disassembly, and the raw QEMU run logs (three runs).

Build (exact command, from the repo root):

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib \
  -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr \
  -mabi=lp64 -mcmodel=medany -Isrc -T link.ld -o jalr-clears-bit0.elf \
  src/boot.S src/uart.c src/jalr-clears-bit0/jalr_main.c
```

Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel jalr-clears-bit0.elf`
(QEMU 8.2.2, `~/workspace/qemu/usr/bin/qemu-system-riscv64`, with the
qemu lib path on LD_LIBRARY_PATH.)

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- Image linked at 0x80000000 (`link.ld`).

## Ground truth (from `bench-logs/jalr-clears-bit0.dis`)

- The label block is at `0x800001f2` (`li t1,1` at `0x800001f2`,
  `sw t1,0(s1)` at `0x800001f4`, `jr t2` at `0x800001f8`).
- The jump is `jalr t2,s0` at `0x800001ec`, where s0 was formed by
  `addi s0,s2,1` at `0x800001e8` from s2 = `0x800001f2`.
- The return path is the `j 0x800001fc` at `0x800001f0`, which the label
  block reaches via `jr t2`.

## Result (from `bench-logs/qemu-run.log`)

```
target: 0x800001f2
odd:    0x800001f3
landed: 1
mismatches: 0
PASS
```

All four checks passed, three runs byte-identical (md5
27d8e494ce968d8ba1bdd193ad9bc41c). The hart was handed the odd address
`0x800001f3` and still ran the block at the even address `0x800001f2`,
then returned. The ISA contract (RISC-V unprivileged manual, RV32I/RV64I
JALR): the target address is obtained by adding the sign-extended 12-bit
immediate to rs1, then setting the least-significant bit of the result
to zero.

Checks: 4. Mismatches: 0.
