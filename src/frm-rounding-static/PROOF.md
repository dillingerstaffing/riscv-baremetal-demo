<!-- PROOF-HEADER
Checks: 29
Mismatches: 0
Checksum: 0x6b500bc19583830
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: a static rm=010 (RDN) in fadd.d overrides fcsr.frm (backlog item "riscv frm-rounding-static")

Backlog item "riscv frm-rounding-static": in M-mode on QEMU, prove
that the 3-bit rm field (bits 14:12) of a floating-point
instruction word selects the rounding mode statically,
overriding fcsr.frm. With rm=010 (RDN) the hardware must round
toward negative infinity even while fcsr.frm reads RNE. Distinct
from the done `src/frm-dynamic-vs-static` item, which uses the
assembler's `, rne` / `, dyn` mnemonics on an fdiv.d; this module
hand-encodes the instruction word as a `.word` and cross-checks
the encoding against GAS, so the test never trusts the assembler
to pick the rounding mode. Distinct from the done
`src/fcsr-frm-roundup` and `src/fcsr-frm-rounddn` items, which
measure the dynamic frm path.

## What was built

`src/frm-rounding-static/`, a bare-metal M-mode binary sharing only
`src/boot.S` and the UART driver with the other demos. It walks
this sequence and checks every step in code:

1. Installs a counting M-mode trap handler (`frs_trap.S`,
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
6. Trial 1 (static): writes `csrw fcsr, 0x00` (frm=RNE,
   fflags=0) and requires the readback to be exactly 0x00.
   Executes one volatile hand-encoded `fadd.d f1, f2, f3` with
   rm=010 (RDN) on the exact-halfway pair 1.0 + 2^-53, and reads
   the result bits back with `fmv.x.d`. Asserts the result bits
   equal the analytically derived expectation (below),
   `fflags == 0x01` (NX set, no other flag), and the frm field
   still reading 0.
7. Trial 2 (static): same hand-encoded instruction on the pair
   1.0 + (2^-53 + 2^-54), whose exact sum is 1 + 0.75*2^-52;
   asserts the lower neighbor 0x3FF0000000000000 with the same
   three assertions.
8. Control A (dynamic): `fadd.d` with rm=111 (DYN, the `, dyn`
   mnemonic) and fcsr.frm = RNE on the trial-2 pair, so the
   hardware rounds with the frm field and must deliver the
   nearer, upper neighbor 0x3FF0000000000001. The divergence
   from trial 2 on identical operands proves the static result
   came from the instruction word's rm field and not from frm.
9. Control B (dynamic): rm=111 (DYN) with fcsr.frm = RDN on the
   trial-1 pair; asserts the same 0x3FF0000000000000 bits the
   static encoding delivered, proving the hand-encoded word
   selected exactly the mode the dynamic path selects under
   frm=RDN.
10. Asserts the neighbor relation between trial 2 and control A:
    q_static_threeq == q_dyn_rne - 1 as unsigned 64-bit values,
    and q_static_threeq < q_dyn_rne.
11. Restores fcsr to its exact boot value and requires the
    readback to match.
12. FS sanity: after all FP writes the mstatus FS field must
    not read Off (the FP state was genuinely touched).
13. The trap counter must be 0.

## The hand encoding, and how it was verified

The executed instruction is built from the field table, not from
a mnemonic:

- funct7 = 0000001 [31:25] (FADD.D)
- rs2 = f3 = 00011 [24:20]
- rs1 = f2 = 00010 [19:15]
- rm = 010 [14:12] (RDN, static)
- rd = f1 = 00001 [11:7]
- opcode = 1010011 [6:0]

which is 0x023120d3. Three independent checks lock this in:

1. A scratch GAS assemble of the mnemonic
   `fadd.d f1, f2, f3, rdn` produced byte-identical output:
   `0: 023120d3 fadd.d ft1,ft2,ft3,rdn`.
2. Bit extraction from the hand word:
   (0x023120d3 >> 12) & 7 = 2 = 010.
3. In the module object, the instruction site is
   `fmv.d.x ft2,a5` / `fmv.d.x ft3,s0` (operands into f2/f3 as
   encoded) immediately followed by `.word 0x023120d3`, then
   `fmv.x.d s0,ft1` (result out of f1 as encoded), all inside one
   volatile asm block so the order is fixed.

Also verified: GAS assembles `fadd.d f1, f2, f3, dyn` as rm=111
(the scratch object showed the second test instruction with the
`,dyn` suffix elided, which is GAS's rendering of the rm=111
DYN form).

## The operand pairs, derived from binary expansions

A double has a 53-bit significand (implicit leading 1 plus 52
stored fraction bits); the representable LSB of 1.0 has weight
2^-52.

Pair A: a = 1.0 = 0x3FF0000000000000, b = 2^-53 =
0x3CA0000000000000 (a power of two, so exactly representable;
exponent field 970 = 0x3CA). The infinitely precise sum
1 + 2^-53 needs a significand bit at 2^-53, exactly one below
the representable LSB, and it is exact in 54-bit precision, so
it sits exactly halfway between the adjacent doubles 1.0
(0x3FF0000000000000) and 1+2^-52 (0x3FF0000000000001), with no
double rounding. Round toward negative infinity of a positive
value must deliver the lower neighbor: 1.0. The operation is
inexact, so NX must be set and no other flag.

Pair B: a = 1.0, b = 2^-53 + 2^-54 = 1.5*2^-53 =
0x3CA8000000000000 (exactly representable: two exact powers of
two, sum exact in binary). The infinitely precise sum is
1 + 0.75*2^-52: strictly between the same two neighbors, 0.75
ulp above the lower one and 0.25 ulp below the upper one, so
round-to-nearest must deliver the upper neighbor
0x3FF0000000000001 and round toward -inf must deliver the lower
neighbor 0x3FF0000000000000.

A note on the tie under RNE, recorded because the original
backlog sketch expected otherwise: on the exact-halfway pair,
round-to-nearest-even picks the candidate with the even LSB,
which is 1.0 (0x3FF0000000000000), the same bits RDN delivers
there. So RNE cannot distinguish the encodings on pair A, and
control A deliberately uses pair B, where RNE and RDN genuinely
differ. The machine confirmed this reasoning: the DYN/RNE trial
on pair B delivered 0x3FF0000000000001, one ulp above the static
result.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/frm-rounding-static/frs_trap.S -o src/frm-rounding-static/frs_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/frm-rounding-static/frs_main.c -o src/frm-rounding-static/frs_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o frm-rounding-static.elf src/boot.o src/uart.o src/frm-rounding-static/frs_trap.o src/frm-rounding-static/frs_main.o
ld: warning: frm-rounding-static.elf has a LOAD segment with RWX permissions
```

The RWX warning comes from the shared `link.ld` (used by every
module in this repo) and is present in the other modules' build
logs too; no new warning was introduced by this module. The
toolchain is the Ubuntu gcc-riscv64-unknown-elf 13.2.0 /
binutils 2.42 packages (the repo's documented working
toolchain).

## Run logs (QEMU 8.2.2, all 3 runs)

Run 1 (run1.log):
```
frm-rounding-static: static rm=010 (RDN) overrides fcsr.frm
setup: mtvec=0x800001c8 mie=0x0
setup: misa=0x80000000001411ad
setup: fcsr(boot)=0x0
add static-RDN (frm=RNE): fcsr-write=0x0 fcsr-readback=0x0 result=0x3ff0000000000000 fcsr=0x1 fflags=0x1 frm=0
add static-RDN (frm=RNE): fcsr-write=0x0 fcsr-readback=0x0 result=0x3ff0000000000000 fcsr=0x1 fflags=0x1 frm=0
add dyn (frm=RNE): fcsr-write=0x0 fcsr-readback=0x0 result=0x3ff0000000000001 fcsr=0x1 fflags=0x1 frm=0
add dyn (frm=RDN): fcsr-write=0x40 fcsr-readback=0x40 result=0x3ff0000000000000 fcsr=0x41 fflags=0x1 frm=2
rel: q_static_threeq=0x3ff0000000000000 q_dyn_rne=0x3ff0000000000001
rel: q_static_half=0x3ff0000000000000 q_dyn_rdn=0x3ff0000000000000
end: fcsr(restored)=0x0
end: mstatus=0x8000000a00006000 FS=3
traps: count=0 mcause=0x0 mepc=0x0 mtval=0x0
checks=29 mismatches=0
checksum=0x6b500bc19583830
RESULT: PASS
```

Runs 2 and 3 (run2.log, run3.log) are byte-identical to run 1.
QEMU exit code 0 on all 3 runs (finisher shutdown path).

## What was verified, in one place

- The hand-encoded word 0x023120d3 (rm=010) executes an
  fadd.d on the intended registers and rounds toward -inf
  while fcsr.frm reads RNE: the exact-halfway pair and the
  0.75-ulp pair both delivered the lower neighbor
  0x3FF0000000000000 with fflags NX alone.
- The same pair under rm=111 (DYN) with frm=RNE delivered the
  upper neighbor 0x3FF0000000000001, exactly one ulp above the
  static result, so the static result was decided by the
  instruction word, not by frm.
- The same halfway pair under rm=111 (DYN) with frm=RDN
  delivered bit-identical 0x3FF0000000000000, so the
  hand-encoded word selected exactly the mode the dynamic path
  selects under frm=RDN.
- fcsr.frm read the trial's mode after every operation (the
  static mode did not leak into frm); fcsr restored to its boot
  value; 0 traps across the whole run.
