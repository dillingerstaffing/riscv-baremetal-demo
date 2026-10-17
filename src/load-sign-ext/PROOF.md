<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: lw sign-extends, lwu zero-extends

## What was built

`src/load-sign-ext/`: a bare-metal RISC-V program that checks, against the
disassembly and the ISA contract, what a 32-bit word load puts into the upper
32 bits of a 64-bit register. Three files, sharing only `src/boot.S` and
`src/uart.c` with the other demos.

- `lse_main.c`: UART bring-up and four checks. Stores the pattern `0x80000001`
  (top bit set) to an aligned scratch word with `sw`, reads it back once with
  `lw` and once with `lwu`, prints both register values, then checks: (1) the
  `lw` result equals `0xffffffff80000001`, (2) the `lwu` result equals
  `0x0000000080000001`, (3) the `lw` result does NOT equal the original 32-bit
  pattern as a 64-bit value (the silent-`==` consequence), (4) the `lwu`
  result does.
- `PROOF.md` (this file), `bench-logs/` with the build log, the full
  disassembly, and the raw QEMU run log.

Build (exact command, from the repo root):

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib \
  -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr \
  -mabi=lp64 -mcmodel=medany -Isrc -T link.ld -o load-sign-ext.elf \
  src/boot.S src/uart.c src/load-sign-ext/lse_main.c
```

Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel load-sign-ext.elf`

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- Image linked at 0x80000000 (`link.ld`).

## Ground truth (from `bench-logs/load-sign-ext.dis`)

- The store is `sw a5,0(a0)` at `0x80000204`.
- The sign-extending load is `lw s1,0(a0)` at `0x80000206`.
- The zero-extending load is `lwu s0,0(a0)` at `0x80000208`.

## Result (from `bench-logs/qemu-run.log`)

```
pattern: 0x80000001
lw  -> 0xffffffff80000001
lwu -> 0x80000001
mismatches: 0
PASS
```

All four checks passed. The `lw` result has bit 31 copied through bits
63..32; the `lwu` result has zeros there. The ISA contract (RISC-V
unprivileged manual, RV64I loads): "The LW instruction loads a 32-bit value
from memory and sign-extends this to 64 bits before storing it in register
rd for RV64I. The LWU instruction, on the other hand, zero-extends the
32-bit value from memory for RV64I."

Checks: 4. Mismatches: 0.
