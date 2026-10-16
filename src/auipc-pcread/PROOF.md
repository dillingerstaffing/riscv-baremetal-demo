<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: auipc reads the program counter

## What was built

`src/auipc-pcread/`: a bare-metal RISC-V program that checks, against the
disassembly, that `auipc` is the one instruction that puts the program
counter's own value into a register. Three files, sharing only
`src/boot.S` and `src/uart.c` with the other demos.

- `auipc_main.c`: UART bring-up and three checks. Check 1 executes
  `auipc rd, 0` and prints the result; check 2 executes
  `auipc rd, 0x12345` and prints the result; check 3 uses the
  `auipc`/`ld` pair with `%pcrel_hi`/`%pcrel_lo` to load a constant
  through a link-time offset from the live PC, with no knowledge of the
  load address.
- `PROOF.md` (this file), `bench-logs/` with the build log, the full
  disassembly, and the raw QEMU run log.

Build (exact command, from the repo root):

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib \
  -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr \
  -mabi=lp64 -mcmodel=medany -Isrc -T link.ld -o auipc-pcread.elf \
  src/boot.S src/uart.c src/auipc-pcread/auipc_main.c
```

Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel auipc-pcread.elf`

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- Image linked at 0x80000000 (`link.ld`).

## Ground truth (from `bench-logs/auipc-pcread.dis`)

- Check 1's `auipc s0, 0` sits at `0x800001d2` (the `auipc a0, 0`
  + `addi` at `0x800001d6` beside it is the compiler's string-address
  computation for the following `uart_puts`, not the test).
- Check 2's `auipc s0, 0x12345` sits at `0x800001f4`.
- Check 3's pair is `auipc t0, 0` at `0x80000216` plus
  `ld s0, 314(t0)`, landing on `0x80000350 <marker>`.

## Observed QEMU output (`bench-logs/qemu-run.log`)

```
auipc rd,0      -> 0x800001d2  (expect the auipc's own address from objdump)
auipc rd,0x12345 -> 0x923451f4  (expect pc + 0x12345000)
pc-relative ld  -> 0x123456789abcdef0  (expect 0x123456789abcdef0)
done
```

## Verdict

- Check 1: printed `0x800001d2`, the address of the `auipc` itself. PASS.
- Check 2: printed `0x923451f4` = `0x800001f4 + 0x12345000`. The
  20-bit immediate lands in the upper 20 bits; the low 12 stay zero. PASS.
- Check 3: the pair loaded `0x123456789abcdef0` from `marker` through
  the PC-relative offset, with the load address named nowhere in the
  instruction stream. PASS.

The program counter is not one of the 32 integer registers and no
instruction takes it as a source operand. `auipc rd, 0` is the one
instruction whose result is the PC itself. Mismatches: 0.
