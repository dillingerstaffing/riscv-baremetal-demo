<!-- PROOF-HEADER
Checks: 15
Mismatches: 0
Checksum: 0x694ad22729ed2d16
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: fflags UF (underflow) accrual on an underflowing fmul.d (backlog item "riscv fflags-uf-underflow")

Backlog item "riscv fflags-uf-underflow": in M-mode on QEMU, clear
`fflags`, run a tiny-times-tiny double multiply, and verify the UF
bit is set, NX is also set (an underflowed result is inexact), and
no other flag bit moves; publish the fcsr read triple across 3
runs. Distinct from the done `src/fflags-nx-inexact` item, which
measured NX accrual on an inexact but non-underflowing `fdiv.d`,
and from `src/frm-rounding-write` / `src/frm-dynamic-vs-static`,
which measured rounding-mode steering, not accrued exception
flags.

## What was built

`src/fflags-uf-underflow/`, a bare-metal M-mode binary sharing only
`src/boot.S` and the UART driver with the other demos. It walks
this sequence and checks every step in code:

1. Installs a counting M-mode trap handler (`ffu_trap.S`, direct
   mode, `mscratch` pointing at the trap record) as a safety net,
   clears `mstatus.MIE`, and asserts `mie == 0` at boot so no
   interrupt can fire.
2. Reads `misa` and requires the F and D extension bits, because
   `fmul.d` is a D-extension instruction.
3. Reads the boot `mstatus` and requires the FS field (bits 14:13)
   to read 0 (Off).
4. Sets FS to Initial (1) with `csrs mstatus`; requires the
   readback FS to be 1 with SD 0 and no other bit changed vs the
   baseline. This step is required before any FP use: with
   FS == Off an FP instruction raises illegal-instruction.
5. Clears the FP exception state with `csrw fcsr, x0` (fflags=0,
   frm=RNE); requires the `fcsr` readback to be exactly `0x00`.
6. Loads the bit pattern for DBL_MIN (`0x0010000000000000`) into
   an integer register, moves it into f2 and f3 with `fmv.d.x`,
   and executes one volatile in-asm `fmul.d f1, f2, f3, rne`. The
   volatile asm keeps the compiler from constant-folding or
   eliminating the multiply. Requires the `fcsr` readback to be
   exactly `0x3`: UF (bit 1) and NX (bit 0) set, NV/DZ/OF all 0,
   frm still RNE.
7. Reads the product back with `fmv.x.d` and requires it to be
   `0x0000000000000000` (the underflowed result rounds to zero).
   This is a sanity anchor only; the verdict rests on the fflags
   checks.
8. Clears `fcsr` again; requires the readback to be exactly
   `0x00`.
9. Requires the trap counter to be 0.

The fcsr read triple (before the multiply, after it, cleared) is
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
register clobber is needed. The multiply's operand words and the
product move through integer registers only, so no host
floating-point arithmetic is involved in feeding the operation.
`src/boot.S` is first in the link order so `_start` lands at
0x80000000.

Build integration: `Makefile` gains `fflags-uf-underflow.elf` and
`run-fflags-uf-underflow`; the module is in `all` and `clean`.

## Operand choice and its IEEE 754 justification

DBL_MIN is the smallest positive normal double, bit pattern
`0x0010000000000000` (exponent field 1, unbiased exponent
emin = -1022). The exact product DBL_MIN * DBL_MIN is 2^-2044.
The exact exponent -2044 is below emin, so the exact result
cannot be represented as a normal double: it is tiny. The
rounded result is 0 (2^-2044 lies far below even the smallest
subnormal double, 2^-1074), and 0 differs from the exact nonzero
2^-2044, so the operation is inexact. A tiny, inexact result is
exactly the underflow condition in IEEE 754, hence UF must set;
an inexact result must set NX. Both claims were checked against
the machine, not assumed: the measured `fcsr` readback is
`0x3`.

A nearby alternative was rejected before shipping: DBL_MIN * 0.5.
Its exact result 2^-1023 is exactly representable as a subnormal
(`0x0008000000000000`), hence exact, hence NOT an underflow;
running it would prove nothing about UF.

## Bit-position correction, grounded by measurement

The first draft of this module asserted the draft expectation
`0x11` (UF at bit 4). The machine answered `fcsr=0x3`, and the
run reported 2 mismatches and exited 124. Per the RISC-V ISA
manual's fflags field, the accrued-exception bits run
NX=bit 0, UF=bit 1, OF=bit 2, DZ=bit 3, NV=bit 4; the observed
`0x3` is UF|NX with OF/DZ/NV clear, exactly the expected
underflow signature. The expectation was corrected to `0x3`
before shipping. The failing run was not discarded silently: it
is the measurement that corrected the draft.

## The instruction in the binary

One occurrence of `fmul.d` exists in the ELF, confirmed by
objdump:

```
800003bc:  123100d3   fmul.d  ft1,ft2,ft3,rne
```

Field decode of `0x123100d3`: opcode `1010011` (OP-FP), rd=1
(f1), rm=`000` (RNE), rs1=2 (f2), rs2=3 (f3), funct7=`0001001`.
That funct7 is FMUL.D per the ISA manual's OP-FP table
(FADD.D is `0000001`, FSUB.D `0000101`, FMUL.D `0001001`,
FDIV.D `0001101`). No other FP arithmetic instruction is
present anywhere in the binary; the operand feeding uses
`fmv.d.x` and the product readback uses `fmv.x.d` only.

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
9. After the underflowing `fmul.d`, the fflags field (bits 4:0)
   reads exactly `0x3`.
10. UF (bit 1) and NX (bit 0) are each individually set in the
    readback.
11. After the `fmul.d`, no non-fflags bit of `fcsr` moved
    (reserved bits and frm unchanged).
12. After the `fmul.d`, frm still reads RNE (0).
13. The product bit pattern read back with `fmv.x.d` is
    `0x0000000000000000` (underflowed to zero).
14. After the second `csrw fcsr, x0`, the `fcsr` readback is
    exactly `0x00`.
15. The trap counter is 0 (no trap fired).

## Measured read sequence

QEMU 8.2.2 (`qemu-system-riscv64 --version` reports
"QEMU emulator version 8.2.2"), `-machine virt`, 3 runs,
byte-identical. `misa` readback: `0x80000000001411ad` (F and D
bits set). `mtvec` readback: `0x800001c8` direct mode; `mie`
readback: `0x0`.

| step               | `fcsr` readback | fflags | frm |
|--------------------|-----------------|--------|-----|
| before (cleared)   | 0x0             | 0x0    | 0   |
| after fmul.d       | 0x3             | 0x3    | 0   |
| cleared            | 0x0             | 0x0    | 0   |

Read triple (fcsr before the multiply, after it, cleared):
`0x0, 0x3, 0x0`. Product bit pattern: `0x0`. Trap record:
count=0, mcause=0x0, mepc=0x0, mtval=0x0. `checks=15
mismatches=0`, `checksum=0x694ad22729ed2d16`, `RESULT: PASS` on
all 3 runs, QEMU exit code 0 on all 3 runs.

## Reading the numbers

- `checks=15` counts the 15 computed assertions above; none is
  an eyeball comparison.
- `mismatches=0` means every assertion held.
- `checksum=0x694ad22729ed2d16` is FNV-1a over the eight
  logged measurement words (fcsr before/after/cleared, product
  bits, baseline and post-`csrs` mstatus, misa, trap count).
  The checksum is over measurement words only, not over the
  pass/fail tally; it ties the header to the exact measured
  values.
- `0x3` after the multiply decomposes as bit 1 (UF) and bit 0
  (NX). Bits 2, 3, 4 (OF, DZ, NV) are 0. Nothing else in the
  64-bit `fcsr` moved: bits 7:5 (frm) still read RNE, and all
  reserved bits read 0.

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
  module's 4-byte `fmul.d` only; a correct run never exercises it
  (counter stayed 0).
- Scope: this module measures UF and NX accrual on one
  underflowing double-precision multiply in M-mode with frm=RNE.
  It does not test the other flag bits (no NV/DZ/OF case was
  generated), other rounding modes, single-precision operations,
  flag behavior under traps, or the subnormal-input vs
  subnormal-output distinctions (the chosen operand pair is
  normal * normal underflowing to zero).

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fflags-uf-underflow/ffu_trap.S -o src/fflags-uf-underflow/ffu_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fflags-uf-underflow/ffu_main.c -o src/fflags-uf-underflow/ffu_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o fflags-uf-underflow.elf src/boot.o src/uart.o src/fflags-uf-underflow/ffu_trap.o src/fflags-uf-underflow/ffu_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: fflags-uf-underflow.elf has a LOAD segment with RWX permissions
```

The module first built with these same steps, then went through one
source correction (the UF bit-position fix: the draft asserted
0x11, the machine answered 0x3, see above), recompiled, and
relinked; the shipped binary is the one from the log above.

The RWX warning comes from the shared `link.ld` (used by every
module in this repo) and is present in the other modules' build
logs too; no new warning was introduced by this module.

## Run logs (QEMU 8.2.2, all 3 runs)

Run 1 (ffu_run1.log):
```
fflags-uf-underflow: underflowing fmul.d must set fflags.UF+NX
setup: mtvec=0x800001c8 mie=0x0
setup: misa=0x80000000001411ad
before: fcsr=0x0 fflags=0x0 frm=0
after : fcsr=0x3 fflags=0x3 frm=0
product bits=0x0
cleared: fcsr=0x0 fflags=0x0 frm=0
traps: count=0 mcause=0x0 mepc=0x0 mtval=0x0
triple fcsr: before=0x0 after=0x3 cleared=0x0
checks=15 mismatches=0
checksum=0x694ad22729ed2d16
RESULT: PASS
```

Runs 2 and 3 (ffu_run2.log, ffu_run3.log) are byte-identical to
run 1 (md5 4ce3629722aa6cea6362237c8214eb9f on all three).
QEMU exit code 0 on all 3 runs (finisher shutdown path).
```

## README

Add one index-form bullet after the fflags-nx-inexact line (line 440).
