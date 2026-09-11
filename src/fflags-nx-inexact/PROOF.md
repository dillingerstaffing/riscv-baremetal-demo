<!-- PROOF-HEADER
Checks: 14
Mismatches: 0
Checksum: 0xb3d407b627e1c2b2
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: fflags NX (inexact) accrual on an inexact fdiv.d (backlog item "riscv fflags-nx-inexact")

Backlog item "riscv fflags-nx-inexact": in M-mode on QEMU, clear
`fflags`, run 1.0/3.0, and verify the NX bit is set with no other
flag moved; publish the fcsr read triples across 3 runs. Distinct
from the done `src/sstatus-fs-dirty` item, which measures FS
dirty-tracking, not accrued exception flags.

## What was built

`src/fflags-nx-inexact/`, a bare-metal M-mode binary sharing only
`src/boot.S` and the UART driver with the other demos. It walks
this sequence and checks every step in code:

1. Installs a counting M-mode trap handler (`fnx_trap.S`, direct
   mode, `mscratch` pointing at the trap record) as a safety net,
   clears `mstatus.MIE`, and asserts `mie == 0` at boot so no
   interrupt can fire.
2. Reads `misa` and requires the F and D extension bits, because
   `fdiv.d` is a D-extension instruction.
3. Reads the boot `mstatus` and requires the FS field (bits 14:13)
   to read 0 (Off).
4. Sets FS to Initial (1) with `csrs mstatus`; requires the
   readback FS to be 1 with SD 0 and no other bit changed vs the
   baseline. This step is required before any FP use: with
   FS == Off an FP instruction raises illegal-instruction.
5. Clears the FP exception state with `csrw fcsr, x0` (fflags=0,
   frm=RNE); requires the `fcsr` readback to be exactly `0x00`.
6. Loads the bit patterns for 1.0 (`0x3FF0000000000000`) and 3.0
   (`0x4008000000000000`) into integer registers, moves them into
   f2/f3 with `fmv.d.x`, and executes one volatile in-asm
   `fdiv.d f1, f2, f3, rne`. The volatile asm keeps the compiler
   from constant-folding or eliminating the divide. Requires the
   `fcsr` readback to be exactly `0x01`: NX set, NV/DZ/OF/UF all
   0, frm still RNE.
7. Reads the quotient back with `fmv.x.d` and requires it to equal
   the correctly rounded 1/3 double, `0x3FD5555555555555`. This is
   a sanity anchor only; the verdict rests on the fflags checks.
8. Clears `fcsr` again; requires the readback to be exactly
   `0x00`.
9. Requires the trap counter to be 0.

The fcsr read triple (before the divide, after it, cleared) is
printed, a 64-bit FNV-1a checksum over the eight logged
measurement words is printed, and `RESULT: PASS` prints only when
every check holds. On PASS the module writes the virt test-device
finisher word `0x5555` at `0x100000`, which shuts the machine down
(QEMU exits 0); on FAIL it parks the hart in a `wfi` loop without
touching the finisher, so under the harness's `timeout` a FAIL is
observable as exit status 124 as well as the `RESULT: FAIL` line.

The FP instructions assemble under in-asm `.option arch, +d`
because the module builds with `-march=rv64imac_zicsr` (no F/D);
the compiler can never allocate f1/f2/f3 under that march, so no
register clobber is needed. The divide's operand words and the
quotient move through integer registers only, so no host
floating-point arithmetic is involved in feeding the operation.
`src/boot.S` is first in the link order so `_start` lands at
0x80000000.

Build integration: `Makefile` gains `fflags-nx-inexact.elf` and
`run-fflags-nx-inexact`; the module is in `all` and `clean`.

## Why the rounding mode is explicit

GAS assembles a bare `fdiv.d f1, f2, f3` with rm=DYN (dynamic
rounding): the first build's objdump line read
`1a3100d3 fdiv.d ft1,ft2,ft3` with the rm field at 0b111. The
source was changed to `fdiv.d f1, f2, f3, rne` so the instruction
in the binary is unambiguous. The shipped binary's objdump line
is:

```
800003c4:  1a3100d3   fdiv.d  ft1,ft2,ft3,rne
```

Field decode of `0x1a3100d3`: opcode `1010011`, rd=1 (f1),
rm=`000` (RNE), rs1=2 (f2), rs2=3 (f3), funct7=`0001101`. That
funct7 is FDIV.D per the ISA manual's OP-FP table (FADD.D is
`0000001`, FSUB.D `0000101`, FMUL.D `0001001`, FDIV.D `0001101`;
verified against an external instruction-format reference before
shipping). One occurrence of this encoding exists in the ELF, at
the address objdump disassembles, and no other FP arithmetic
instruction is present anywhere in the binary.

## Checks (all computed, none eyeballed)

1. `mtvec` took the handler address.
2. `mtvec` is in direct mode (base[1:0] clear).
3. `mie == 0` at boot.
4. `misa` carries the F (bit 5) and D (bit 3) extension bits.
5. Boot `mstatus` FS reads 0 (Off).
6. After `csrs`, FS reads 1 (Initial).
7. After `csrs`, no non-FS bit changed vs the baseline and SD
   reads 0.
8. After `csrw fcsr, x0`, the `fcsr` readback is exactly `0x00`.
9. After the inexact `fdiv.d`, the fflags field (bits 4:0) reads
   exactly `0x1` (NX set, NV/DZ/OF/UF all 0).
10. After the `fdiv.d`, no non-fflags bit of `fcsr` moved
    (reserved bits and frm unchanged).
11. After the `fdiv.d`, frm still reads RNE (0).
12. The quotient bit pattern read back with `fmv.x.d` equals
    `0x3FD5555555555555` (the correctly rounded 1/3 double).
13. After the second `csrw fcsr, x0`, the `fcsr` readback is
    exactly `0x00`.
14. The trap counter is 0 (no trap fired).

## Measured read sequence

QEMU 8.2.2 (`qemu-system-riscv64 --version` reports
"QEMU emulator version 8.2.2"), `-machine virt`, 3 runs,
byte-identical. `misa` readback: `0x80000000001411ad` (F and D
bits set). `mtvec` readback: `0x800001c8` direct mode; `mie`
readback: `0x0`.

| step             | `fcsr` readback | fflags | frm |
|------------------|-----------------|--------|-----|
| before (cleared) | 0x0             | 0x0    | 0   |
| after fdiv.d     | 0x1             | 0x1    | 0   |
| cleared          | 0x0             | 0x0    | 0   |

Read triple (fcsr before the divide, after it, cleared):
`0x0, 0x1, 0x0`. Quotient bit pattern:
`0x3fd5555555555555`. Trap record: count=0, mcause=0x0,
mepc=0x0, mtval=0x0. `checks=14 mismatches=0`,
`checksum=0xb3d407b627e1c2b2`, `RESULT: PASS` on all 3 runs,
QEMU exit code 0 on all 3 runs.

Every value above is deterministic across runs; nothing in the
measurement words depends on host timing (the only
timing-dependent read, `mtime`, is used solely as a UART-drain
timebase and is not part of the checksum).

## Limits of verification

- Emulator, not silicon: every number above is a property of QEMU
  8.2.2's FP and CSR implementation on this host, not of physical
  RISC-V hardware. A real core's accrued-exception behavior is
  defined by the same spec text but was not measured here.
- The trap handler's mepc+4 skip is a safety net for this
  module's 4-byte `fdiv.d` only; a correct run never exercises it
  (counter stayed 0).
- Scope: this module measures NX accrual on one inexact
  double-precision divide in M-mode with frm=RNE. It does not
  test the other flag bits (no NV/DZ/OF/UF case was generated),
  other rounding modes, single-precision operations, or
  flag behavior under traps.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fflags-nx-inexact/fnx_trap.S -o src/fflags-nx-inexact/fnx_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fflags-nx-inexact/fnx_main.c -o src/fflags-nx-inexact/fnx_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o fflags-nx-inexact.elf src/boot.o src/uart.o src/fflags-nx-inexact/fnx_trap.o src/fflags-nx-inexact/fnx_main.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/../lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: fflags-nx-inexact.elf has a LOAD segment with RWX permissions
```

The RWX warning comes from the shared `link.ld` (used by every
module in this repo) and is present in the other modules' build
logs too; no new warning was introduced by this module.

## Run logs (QEMU 8.2.2, all 3 runs)

Run 1 (run1.log):
```
fflags-nx-inexact: inexact fdiv.d must set fflags.NX
setup: mtvec=0x800001c8 mie=0x0
setup: misa=0x80000000001411ad
before: fcsr=0x0 fflags=0x0 frm=0
after : fcsr=0x1 fflags=0x1 frm=0
quotient bits=0x3fd5555555555555
cleared: fcsr=0x0 fflags=0x0 frm=0
traps: count=0 mcause=0x0 mepc=0x0 mtval=0x0
triple fcsr: before=0x0 after=0x1 cleared=0x0
checks=14 mismatches=0
checksum=0xb3d407b627e1c2b2
RESULT: PASS
```

Runs 2 and 3 (run2.log, run3.log) are byte-identical to run 1.
QEMU exit code 0 on all 3 runs (finisher shutdown path).
