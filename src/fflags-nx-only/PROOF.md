<!-- PROOF-HEADER
Checks: 18
Mismatches: 0
Checksum: 0x6debdcf01d3a7d65
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: one inexact fadd.d sets fflags.NX alone; an exact fadd.d sets nothing (backlog item "riscv fflags-nx-only")

Backlog item "riscv fflags-nx-only": in M-mode on QEMU, prove
that `fflags.NX` is the accrued inexact flag, so a single
inexact floating-point operation must set NX and move no other
accrued flag. The inexact phase runs `fadd.d(1e16, 1.0)`: the
exact sum needs 54 significant bits and is not representable,
so `fcsr` must read exactly `0x01` afterward (NX set; NV, DZ,
OF, UF clear). The exact control phase runs `fadd.d(1.0, 2.0) =
3.0`: the exact sum is representable, so `fcsr` must read
exactly `0x00` afterward. Publish the result/fcsr hex pairs
for both phases across 3 runs. Distinct from the done
`src/fflags-nx-inexact` item, which measures NX accrual on an
inexact `fdiv.d` only, and from the `fflags-uf/of/dz/nv`
items, which each measure one other flag; this module isolates
the NX-only invariant (nothing else moves on an inexact add)
and anchors it against an exact-add control (nothing moves at
all).

## What was built

`src/fflags-nx-only/`, a bare-metal M-mode binary sharing only
`src/boot.S` and the UART driver with the other demos. It walks
this sequence and checks every step in code:

1. Installs a counting M-mode trap handler (`fnxo_trap.S`,
   direct mode, `mscratch` pointing at the trap record) as a
   safety net, clears `mstatus.MIE`, and asserts `mie == 0` at
   boot so no interrupt can fire.
2. Reads `misa` and requires the F and D extension bits,
   because the sequence executes `fadd.d`, a D-extension
   instruction.
3. Reads the boot `mstatus` and requires the FS field (bits
   14:13) to read 0 (Off).
4. Sets FS to Initial (1) with `csrs mstatus`; requires the
   readback FS to be 1 with SD 0 and no other bit changed vs
   the baseline. This step is required before any FP use: with
   FS == Off an FP instruction raises illegal-instruction.
5. Records the boot `fcsr` value (0x0) so the run can restore it
   exactly at the end.
6. Phase A (inexact): writes `csrw fcsr, 0x00` (frm=RNE,
   fflags=0) and requires the readback to be exactly 0x00.
   Executes one volatile in-asm `fadd.d f1, f2, f3, dyn`
   (rm=111, dynamic rounding, so the instruction rounds with
   the frm field) on the operand pair (1e16, 1.0), and reads
   the result bits back with `fmv.x.d`. Asserts the result
   bits equal the analytically derived expectation (below),
   `(fcsr & 0x1f) == 0x01` (NX set, NV/DZ/OF/UF clear), and
   the frm field still reading 0.
7. Phase B (exact control): writes `csrw fcsr, 0x00` and
   requires the readback to be exactly 0x00. Executes the same
   instruction shape on (1.0, 2.0); asserts the result bits
   equal 3.0, `(fcsr & 0x1f) == 0x00` (no accrued flag moved),
   and the frm field still reading 0.
8. Restores `fcsr` to its boot value with one write and requires
   the readback to match exactly.
9. FS readback sanity: after the FP writes, `mstatus` FS must
   not read Off (the FP state was genuinely touched).
10. Requires the trap counter to be 0.

The result/fcsr pairs are printed with their fcsr-write,
fcsr-readback, fflags, and frm decodes, a 64-bit FNV-1a
checksum over the logged measurement words is printed, and
`RESULT: PASS` prints only when every check holds. On PASS the
module writes the virt test-device finisher word `0x5555` at
`0x100000`, which shuts the machine down (QEMU exits 0); on
FAIL it parks the hart in a `wfi` loop without touching the
finisher, so under the harness's `timeout` a FAIL is
observable as exit status 124 as well as the `RESULT: FAIL`
line.

The operands are loaded as bit patterns (`0x4341C37937E08000`
for 1e16, `0x3FF0000000000000` for 1.0,
`0x4000000000000000` for 2.0) moved into FP registers with
`fmv.d.x` from integer registers, and the results are read
back with `fmv.x.d`, so no host floating point is involved in
feeding the operations or reading their results. `src/boot.S`
is first in the link order so `_start` lands at 0x80000000.

Build integration: `Makefile` gains `fflags-nx-only.elf` and
`run-fflags-nx-only`; the module is in `all` and `clean`.

## The dynamic rounding mode is explicit in the shipped binary

GAS assembles `fadd.d f1, f2, f3, dyn` with rm=111 (dynamic
rounding). The shipped binary's objdump line is:

```
800002b0:  023170d3   fadd.d  ft1,ft2,ft3
```

Field decode of `0x023170d3`: opcode `1010011`, rd=1 (f1),
rm=`111` (DYN), rs1=2 (f2), rs2=3 (f3), bits 31:25=`0000001`
(FADD funct5 `00000`, fmt `01` = D). The disassembler omits
the rm suffix; the rm field reads 111, which is what makes
each phase's instruction consult the frm field the phase just
wrote (RNE, from `fcsr=0x00`).

## Independent ground truth: the analytic derivation

Phase A: `fadd.d(1e16, 1.0)` under RNE.

10^16 factors as 2^16 * 5^16, and 5^16 = 152587890625, so
1e16 is exactly a double: exponent field 0x434 (e = 53),
53-bit significand 152587890625 * 2^15 = 5000000000000000000,
whose least significant bit is 0 (even). 10^16 lies in the
binade [2^53, 2^54), where the ulp is 2, so the representable
neighbors of the exact sum 10^16 + 1 are 10^16 (significand
even) and 10^16 + 2 (significand odd). The exact sum sits
exactly halfway between them; RNE ties-to-even picks the even
significand, i.e. 10^16, bits `0x4341C37937E08000`. The exact
sum needs 54 significant bits and is not representable, so the
operation is inexact and NX must set.

No other flag can move: both operands are finite positive
normals, the result is finite and nowhere near the subnormal
range or the overflow threshold, and no division or invalid
operation is involved, so NV, DZ, OF, and UF have no cause to
set.

Phase B: `fadd.d(1.0, 2.0) = 3.0`. 3.0 = 1.5 * 2^1 is exactly
representable (`0x4008000000000000`); the exact sum is the
delivered sum, so the operation is exact and no accrued flag
may move.

## Checks (all computed, none eyeballed)

Setup (7): `mtvec` took the handler address; `mtvec` is in
direct mode; `mie == 0` at boot; `misa` carries the F and D
extension bits; boot `mstatus` FS reads 0 (Off); after `csrs`,
FS reads 1 (Initial); after `csrs`, no non-FS bit changed and
SD reads 0.

Phase A (4): `fcsr` readback is exactly 0x00 after the clear
write; the result bits equal `0x4341c37937e08000`;
`(fcsr & 0x1f) == 0x01` (NX set, no other flag); the frm field
still reads 0.

Phase B (4): `fcsr` readback is exactly 0x00 after the clear
write; the result bits equal `0x4008000000000000` (3.0);
`(fcsr & 0x1f) == 0x00` (no accrued flag moved); the frm field
still reads 0.

Final (3): `fcsr` restored to its boot value 0x00 with an exact
readback; after the FP writes, `mstatus` FS is not Off; the
trap counter is 0.

## Measured result/fcsr pairs

QEMU 8.2.2 (`qemu-system-riscv64 --version` reports
"QEMU emulator version 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18)"),
`-machine virt`, 3 runs, byte-identical. `misa` readback:
`0x80000000001411ad` (F and D bits set). `mtvec` readback:
`0x800001c8` direct mode; `mie` readback: `0x0`. Boot `fcsr`
readback: `0x0`.

| phase | fcsr write | fcsr readback | result bits        | fcsr after | fflags | frm |
|-------|------------|---------------|--------------------|------------|--------|-----|
| A (inexact) | 0x0 | 0x0        | 0x4341c37937e08000 | 0x1        | 0x1    | 0   |
| B (exact)   | 0x0 | 0x0        | 0x4008000000000000 | 0x0        | 0x0    | 0   |

Phase A moves NX alone: `fflags=0x1` with NV/DZ/OF/UF clear.
Phase B moves nothing: `fflags=0x0`. `fcsr` restored to `0x0`
with an exact readback. The end-of-run `mstatus` readback:
`0x8000000a00006000`, FS=3 (Dirty), consistent with the FP
writes having genuinely touched the FP state. Trap record:
count=0, mcause=0x0, mepc=0x0, mtval=0x0. `checks=18
mismatches=0`, `checksum=0x6debdcf01d3a7d65`,
`RESULT: PASS` on all 3 runs, QEMU exit code 0 on all 3 runs
(finisher shutdown path).

Every value above is deterministic across runs; nothing in the
measurement words depends on host timing (the only
timing-dependent read, `mtime`, is used solely as a UART-drain
timebase and is not part of the checksum).

The phase A result equals the 1e16 operand's own bit pattern:
RNE ties-to-even on the exact-halfway sum picks the even
significand, which is 1e16 itself, and NX still sets because
the exact sum (one ulp off, in the unrepresentable middle) was
not delivered. That is the NX-only invariant in its sharpest
form: the operation rounds, nothing else happens.

## Limits of verification

- Emulator, not silicon: every number above is a property of
  QEMU 8.2.2's FP and CSR implementation on this host, not of
  physical RISC-V hardware. A real core's fflags accrual is
  defined by the same spec text but was not measured here.
- The trap handler's mepc+4 skip is a safety net for this
  module's 4-byte FP/CSR instructions only; a correct run never
  exercises it (counter stayed 0).
- Scope: this module measures that one inexact `fadd.d` sets
  NX and no other accrued flag, and one exact `fadd.d` sets
  none, in M-mode on QEMU. It does not test the other accrued
  flags' setters (covered by the `fflags-uf/of/dz/nv` items),
  single-precision operations, or accrual across traps.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fflags-nx-only/fnxo_trap.S -o src/fflags-nx-only/fnxo_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fflags-nx-only/fnxo_main.c -o src/fflags-nx-only/fnxo_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o fflags-nx-only.elf src/boot.o src/uart.o src/fflags-nx-only/fnxo_trap.o src/fflags-nx-only/fnxo_main.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/../lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: fflags-nx-only.elf has a LOAD segment with RWX permissions
```

The RWX warning comes from the shared `link.ld` (used by every
module in this repo) and is present in the other modules' build
logs too; no new warning was introduced by this module. The
toolchain is the Ubuntu gcc-riscv64-unknown-elf 13.2.0 /
binutils 2.42 packages (the repo's documented working
toolchain). Zero compiler warnings under `-Wall -Wextra`.

## Run logs (QEMU 8.2.2, all 3 runs)

Run 1 (run1.log):
```
fflags-nx-only: one inexact fadd.d sets NX alone, an exact fadd.d sets nothing
setup: mtvec=0x800001c8 mie=0x0
setup: misa=0x80000000001411ad
setup: fcsr(boot)=0x0
A: fcsr-write=0x0 fcsr-readback=0x0 result=0x4341c37937e08000 fcsr=0x1 fflags=0x1 frm=0
B: fcsr-write=0x0 fcsr-readback=0x0 result=0x4008000000000000 fcsr=0x0 fflags=0x0 frm=0
end: fcsr(restored)=0x0
end: mstatus=0x8000000a00006000 FS=3
traps: count=0 mcause=0x0 mepc=0x0 mtval=0x0
checks=18 mismatches=0
checksum=0x6debdcf01d3a7d65
RESULT: PASS
```

Runs 2 and 3 (run2.log, run3.log) are byte-identical to run 1.
QEMU exit code 0 on all 3 runs (finisher shutdown path).
