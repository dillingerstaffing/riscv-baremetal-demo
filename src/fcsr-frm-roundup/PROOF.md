<!-- PROOF-HEADER
Checks: 24
Mismatches: 0
Checksum: 0x6585221d7c3874c3
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: the fcsr frm field selects the rounding direction of inexact FP results (backlog item "riscv fcsr-frm-roundup")

Backlog item "riscv fcsr-frm-roundup": in M-mode on QEMU, prove
that the `frm` field of the `fcsr` CSR (bits 7:5) chooses the
rounding function applied to inexact floating-point results. An
FP instruction whose rm field is DYN (111) must round with the
function frm names, so the same exact-halfway addition must
deliver different bit patterns under RNE and RUP, and the
operation must set only the NX flag. Publish the
result/fcsr pairs for each rounding mode across 3 runs.
Distinct from the done `src/frm-rounding-write` item, which
measures frm write/readback for all five modes, and from the
done `src/fcsr-field-independence` item, which measures the
cross-field independence of fflags and frm; this module measures
that frm actually changes the delivered result bits of an
inexact operation.

## What was built

`src/fcsr-frm-roundup/`, a bare-metal M-mode binary sharing only
`src/boot.S` and the UART driver with the other demos. It walks
this sequence and checks every step in code:

1. Installs a counting M-mode trap handler (`fru_trap.S`, direct
   mode, `mscratch` pointing at the trap record) as a safety net,
   clears `mstatus.MIE`, and asserts `mie == 0` at boot so no
   interrupt can fire.
2. Reads `misa` and requires the F and D extension bits, because
   the sequence executes `fadd.d` and `fdiv.d`, D-extension
   instructions.
3. Reads the boot `mstatus` and requires the FS field (bits 14:13)
   to read 0 (Off).
4. Sets FS to Initial (1) with `csrs mstatus`; requires the
   readback FS to be 1 with SD 0 and no other bit changed vs the
   baseline. This step is required before any FP use: with
   FS == Off an FP instruction raises illegal-instruction.
5. Vector 1, three trials: `fadd.d(1.0, 2^-53)`. Each trial
   writes `csrw fcsr, (frm<<5)` (sets the frm field and clears
   fflags in one write), executes one volatile in-asm
   `fadd.d f1, f2, f3, dyn` (rm=111, dynamic rounding, so the
   instruction rounds with the frm field), and reads the result
   bits back with `fmv.x.d`. Per trial it asserts the result
   bits equal the analytically derived expectation (below),
   `fflags == 0x01` (NX set, no other flag), and the frm field
   still reads the trial's mode. Trials: RNE, RNZ, RUP.
6. Vector 2, two trials: `fdiv.d(1.0, 3.0)`, same plumbing with
   `fdiv.d f1, f2, f3, dyn`. Trials: RNE, RUP, with the same
   three assertions per trial.
7. FS readback sanity: after the FP writes, `mstatus` FS must
   not read Off (the FP state was genuinely touched).
8. Requires the trap counter to be 0.

The five result/fcsr pairs are printed with their fflags/frm
decodes, a 64-bit FNV-1a checksum over the logged measurement
words is printed, and `RESULT: PASS` prints only when every
check holds. On PASS the module writes the virt test-device
finisher word `0x5555` at `0x100000`, which shuts the machine
down (QEMU exits 0); on FAIL it parks the hart in a `wfi` loop
without touching the finisher, so under the harness's
`timeout` a FAIL is observable as exit status 124 as well as the
`RESULT: FAIL` line.

The operands are loaded as bit patterns (`0x3FF0000000000000`
for 1.0, `0x3CA0000000000000` for 2^-53, `0x4008000000000000`
for 3.0) moved into FP registers with `fmv.d.x` from integer
registers, and the results are read back with `fmv.x.d`, so no
host floating point is involved in feeding the operations or
reading their results. `src/boot.S` is first in the link order
so `_start` lands at 0x80000000.

Build integration: `Makefile` gains `fcsr-frm-roundup.elf` and
`run-fcsr-frm-roundup`; the module is in `all` and `clean`.

## The dynamic rounding mode is explicit in the shipped binary

GAS assembles `fadd.d f1, f2, f3, dyn` with rm=111 (dynamic
rounding). The shipped binary's objdump lines are:

```
8000020c:  023170d3   fadd.d  ft1,ft2,ft3
8000021e:  1a3170d3   fdiv.d  ft1,ft2,ft3
```

Field decode of `0x023170d3`: opcode `1010011`, rd=1 (f1),
rm=`111` (DYN), rs1=2 (f2), rs2=3 (f3), funct7=`0000101`
(FADD.D). Field decode of `0x1a3170d3`: rm=`111` (DYN),
funct7=`0001101` (FDIV.D). The disassembler omits the rm
suffix; the rm field reads 111 in both encodings, which is what
makes each trial's instruction consult the frm field the trial
just wrote. The binary also carries `csrw fcsr, a5` at
`0x8000028e` (the per-trial frm write) and `csrr s5, fcsr` at
`0x8000029c` (the fcsr readback).

## Independent ground truth: the analytic derivations

Vector 1: `fadd.d(1.0, 2^-53)`.

The bit pattern `0x3FF0000000000000` is sign 0, exponent field
1023 (0x3FF), all-zero fraction: the double 1.0 exactly. The
bit pattern `0x3CA0000000000000` is sign 0, exponent field 970
(0x3CA), all-zero fraction: 2^(970-1023) = 2^-53 exactly.

Doubles with exponent field 1023 have ulp 2^-52. The two
representable neighbors of the exact sum 1 + 2^-53 are 1.0
(fraction all zero) and 1 + 2^-52 (fraction LSB 1). The exact
sum differs from 1.0 by 2^-53, which is exactly half the 2^-52
gap, so the exact sum is exactly halfway between the two
candidates: it is not representable, so NX must set under every
rounding mode.

- RNE (frm=0): ties go to the candidate whose significand LSB
  is even. 1.0's fraction LSB is 0 (even); 1+2^-52's is 1
  (odd). Expect `0x3FF0000000000000`.
- RNZ (frm=1): round toward zero. The exact sum is positive,
  so the candidate toward zero is the smaller one, 1.0.
  Expect `0x3FF0000000000000`.
- RUP (frm=3): round toward +inf. The exact sum is positive,
  so the candidate toward +inf is the larger one, 1+2^-52.
  Expect `0x3FF0000000000001`.

Vector 2: `fdiv.d(1.0, 3.0)`.

1/3 in binary is 0.01010101... repeating "01". Normalized:
2^-2 x 1.010101... with the fraction "01" repeating forever.
A double with exponent 2^-2 has ulp 2^-54. The lower candidate
`0x3FD5555555555555` has exponent field 0x3FD = 1021
(2^(1021-1023) = 2^-2) and fraction `0x5555555555555`, whose 52
bits are "01" repeated 26 times: exactly the first 52 bits of
the exact expansion. So lower = truncate(exact), and the exact
1/3 = lower + (0.01010101... x 2^-54) = lower + (1/3) x 2^-54,
because the infinite tail 0.010101... in binary equals 1/3
(0.010101..._2 = sum 2^-(2k) = 1/3). The upper candidate is
lower + one ulp.

Distance from the exact 1/3 to the lower candidate: (1/3) of
an ulp. Distance to the upper candidate: (2/3) of an ulp. The
exact value is strictly closer to the lower candidate.

- RNE (frm=0): the nearer candidate wins.
  Expect `0x3FD5555555555555`.
- RUP (frm=3): round toward +inf. The exact 1/3 is positive and
  inexact, so the candidate toward +inf is the upper one.
  Expect `0x3FD5555555555556`.

Both operations are inexact, so NX must set; no other flag can
move (no invalid operand, no division by zero, no overflow, not
tiny).

## Checks (all computed, none eyeballed)

Setup (7): `mtvec` took the handler address; `mtvec` is in
direct mode; `mie == 0` at boot; `misa` carries the F and D
extension bits; boot `mstatus` FS reads 0 (Off); after `csrs`,
FS reads 1 (Initial); after `csrs`, no non-FS bit changed and
SD reads 0.

Per trial (3 each, 15 total): the result bits equal the
analytically derived expectation; `fflags == 0x01` (NX set, no
other flag); the frm field still reads the trial's mode.

Final (2): after the FP writes, `mstatus` FS is not Off; the
trap counter is 0.

## Measured result/fcsr pairs

QEMU 8.2.2 (`qemu-system-riscv64 --version` reports
"QEMU emulator version 8.2.2"), `-machine virt`, 3 runs,
byte-identical. `misa` readback: `0x80000000001411ad` (F and D
bits set). `mtvec` readback: `0x800001c8` direct mode; `mie`
readback: `0x0`.

| trial      | frm | result bits          | `fcsr` readback | fflags | frm readback |
|------------|-----|----------------------|-----------------|--------|--------------|
| add (RNE)  | 0   | 0x3ff0000000000000   | 0x1             | 0x1    | 0            |
| add (RNZ)  | 1   | 0x3ff0000000000000   | 0x21            | 0x1    | 1            |
| add (RUP)  | 3   | 0x3ff0000000000001   | 0x61            | 0x1    | 3            |
| div (RNE)  | 0   | 0x3fd5555555555555   | 0x1             | 0x1    | 0            |
| div (RUP)  | 3   | 0x3fd5555555555556   | 0x61            | 0x1    | 3            |

The end-of-run `mstatus` readback: `0x8000000a00006000`,
FS=3 (Dirty), consistent with the FP writes having genuinely
touched the FP state. Trap record: count=0, mcause=0x0,
mepc=0x0, mtval=0x0. `checks=24 mismatches=0`,
`checksum=0x6585221d7c3874c3`, `RESULT: PASS` on all 3 runs,
QEMU exit code 0 on all 3 runs.

Every value above is deterministic across runs; nothing in the
measurement words depends on host timing (the only
timing-dependent read, `mtime`, is used solely as a UART-drain
timebase and is not part of the checksum).

The RNE/RUP split on the exact-halfway addition is the whole
point: the only thing that changed between the two trials was
the frm field, written with a single `csrw fcsr`, and the
delivered bit pattern moved from `0x3ff0000000000000` to
`0x3ff0000000000001`. Likewise the divide moved from
`0x3fd5555555555555` to `0x3fd5555555555556`.

## Limits of verification

- Emulator, not silicon: every number above is a property of
  QEMU 8.2.2's FP and CSR implementation on this host, not of
  physical RISC-V hardware. A real core's frm rounding is
  defined by the same spec text but was not measured here.
- The trap handler's mepc+4 skip is a safety net for this
  module's 4-byte instructions only; a correct run never
  exercises it (counter stayed 0).
- Scope: this module measures that the frm field selects the
  rounding direction for `fadd.d` and `fdiv.d` with rm=DYN in
  M-mode on QEMU. It does not test the other rounding modes
  (RDN, RMM), single-precision operations, or rounding behavior
  across traps.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fcsr-frm-roundup/fru_trap.S -o src/fcsr-frm-roundup/fru_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fcsr-frm-roundup/fru_main.c -o src/fcsr-frm-roundup/fru_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o fcsr-frm-roundup.elf src/boot.o src/uart.o src/fcsr-frm-roundup/fru_trap.o src/fcsr-frm-roundup/fru_main.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/../lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: fcsr-frm-roundup.elf has a LOAD segment with RWX permissions
```

The RWX warning comes from the shared `link.ld` (used by every
module in this repo) and is present in the other modules' build
logs too; no new warning was introduced by this module.

## Run logs (QEMU 8.2.2, all 3 runs)

Run 1 (run1.log):
```
fcsr-frm-roundup: frm selects the rounding direction of inexact FP results
setup: mtvec=0x800001c8 mie=0x0
setup: misa=0x80000000001411ad
add RNE: result=0x3ff0000000000000 fcsr=0x1 fflags=0x1 frm=0
add RNZ: result=0x3ff0000000000000 fcsr=0x21 fflags=0x1 frm=1
add RUP: result=0x3ff0000000000001 fcsr=0x61 fflags=0x1 frm=3
div RNE: result=0x3fd5555555555555 fcsr=0x1 fflags=0x1 frm=0
div RUP: result=0x3fd5555555555556 fcsr=0x61 fflags=0x1 frm=3
end: mstatus=0x8000000a00006000 FS=3
traps: count=0 mcause=0x0 mepc=0x0 mtval=0x0
checks=24 mismatches=0
checksum=0x6585221d7c3874c3
RESULT: PASS
```

Runs 2 and 3 (run2.log, run3.log) are byte-identical to run 1.
QEMU exit code 0 on all 3 runs (finisher shutdown path).
