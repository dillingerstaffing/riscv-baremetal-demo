<!-- PROOF-HEADER
Checks: 20
Mismatches: 0
Checksum: 0xa92c23f3ba9e7afc
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: under frm=RDN an inexact fsub.d rounds toward -inf (backlog item "riscv fcsr-frm-rounddn")

Backlog item "riscv fcsr-frm-rounddn": in M-mode on QEMU, prove
that the `frm` field of the `fcsr` CSR (bits 7:5) chooses the
rounding function applied to inexact floating-point results. An
FP instruction whose rm field is DYN (111) must round with the
function frm names, so the same inexact subtraction must deliver
the lower neighbor under RDN and the upper neighbor under RNE,
with the RDN result exactly one ulp below the RNE result, and
the operation must set only the NX flag. Publish the
result/fcsr pairs for each rounding mode across 3 runs.
Distinct from the done `src/fcsr-frm-roundup` item, which
measures frm selecting RNE/RNZ/RUP on an addition and a
division, and from the done `src/frm-rdn-vs-rup-div` item, which
compares RDN against RUP on a division; this module measures
frm=RDN on a subtraction and requires the exact fcsr write
readback (0x40) plus the one-ulp neighbor relation between the
RDN and RNE results.

## What was built

`src/fcsr-frm-rounddn/`, a bare-metal M-mode binary sharing only
`src/boot.S` and the UART driver with the other demos. It walks
this sequence and checks every step in code:

1. Installs a counting M-mode trap handler (`frd_trap.S`,
   direct mode, `mscratch` pointing at the trap record) as a
   safety net, clears `mstatus.MIE`, and asserts `mie == 0` at
   boot so no interrupt can fire.
2. Reads `misa` and requires the F and D extension bits,
   because the sequence executes `fsub.d`, a D-extension
   instruction.
3. Reads the boot `mstatus` and requires the FS field (bits
   14:13) to read 0 (Off).
4. Sets FS to Initial (1) with `csrs mstatus`; requires the
   readback FS to be 1 with SD 0 and no other bit changed vs
   the baseline. This step is required before any FP use: with
   FS == Off an FP instruction raises illegal-instruction.
5. Records the boot `fcsr` value (0x0) so the run can restore it
   exactly at the end.
6. Trial RDN: writes `csrw fcsr, 0x40` (frm=RDN, fflags=0) and
   requires the readback to be exactly 0x40. Executes one
   volatile in-asm `fsub.d f1, f2, f3, dyn` (rm=111, dynamic
   rounding, so the instruction rounds with the frm field) on
   the operand pair (1 + 3*2^-52) - 2^-55, and reads the result
   bits back with `fmv.x.d`. Asserts the result bits equal the
   analytically derived expectation (below),
   `fflags == 0x01` (NX set, no other flag), and the frm field
   still reading 2.
7. Trial RNE: writes `csrw fcsr, 0x00` and requires the readback
   to be exactly 0x00. Same subtraction, same three assertions,
   plus the neighbor relation between the two trials' results:
   `q_rdn == q_rne - 1` as unsigned 64-bit values, and
   `q_rdn < q_rne`.
8. Restores `fcsr` to its boot value with one write and requires
   the readback to match exactly.
9. FS readback sanity: after the FP writes, `mstatus` FS must
   not read Off (the FP state was genuinely touched).
10. Requires the trap counter to be 0.

The result/fcsr pairs are printed with their fcsr-write,
fcsr-readback, fflags, and frm decodes, the neighbor relation
is printed, a 64-bit FNV-1a checksum over the logged measurement
words is printed, and `RESULT: PASS` prints only when every
check holds. On PASS the module writes the virt test-device
finisher word `0x5555` at `0x100000`, which shuts the machine
down (QEMU exits 0); on FAIL it parks the hart in a `wfi` loop
without touching the finisher, so under the harness's
`timeout` a FAIL is observable as exit status 124 as well as the
`RESULT: FAIL` line.

The operands are loaded as bit patterns (`0x3FF0000000000003`
and `0x3C80000000000000`) moved into FP registers with
`fmv.d.x` from integer registers, and the results are read back
with `fmv.x.d`, so no host floating point is involved in
feeding the operations or reading their results. `src/boot.S`
is first in the link order so `_start` lands at 0x80000000.

Build integration: `Makefile` gains `fcsr-frm-rounddn.elf` and
`run-fcsr-frm-rounddn`; the module is in `all` and `clean`.

## The dynamic rounding mode is explicit in the shipped binary

GAS assembles `fsub.d f1, f2, f3, dyn` with rm=111 (dynamic
rounding). The shipped binary's objdump line is:

```
800002dc:  0a3170d3   fsub.d  ft1,ft2,ft3
```

Field decode of `0x0a3170d3`: opcode `1010011`, rd=1 (f1),
rm=`111` (DYN), rs1=2 (f2), rs2=3 (f3), funct7=`0000101`
(FSUB.D: bits 31:27 = `00001` FSUB, bits 26:25 = `01` fmt D).
The disassembler omits the rm suffix; the rm field reads 111,
which is what makes each trial's instruction consult the frm
field the trial just wrote. The binary also carries
`csrw fcsr, a0` at `0x80000260` (the per-trial frm write) and
`csrr s4, fcsr` at `0x80000264` (the fcsr readback).

## Independent ground truth: the analytic derivation

Operands: `fsub.d(1 + 3*2^-52, 2^-55)`.

The bit pattern `0x3FF0000000000003` is sign 0, exponent field
1023 (0x3FF), fraction 0x3: the double 1 + 3*2^-52 exactly.
The bit pattern `0x3C80000000000000` is sign 0, exponent field
968 (0x3C8), all-zero fraction: 2^(968-1023) = 2^-55 exactly.

Doubles with exponent field 1023 (the binade [1, 2)) have ulp
2^-52. The exact difference is 1 + 3*2^-52 - 2^-55. Since
2^-52 = 8*2^-55, 3*2^-52 = 24*2^-55, and 24 - 1 = 23, the
exact difference is 1 + 23*2^-55 = 1 + (23/8)*2^-52 =
1 + 2.875*2^-52.

The two representable neighbors of the exact difference are
1 + 2*2^-52 (`0x3FF0000000000002`) and 1 + 3*2^-52
(`0x3FF0000000000003`). 2.875 lies strictly between 2 and 3,
so the exact difference sits strictly between the two
candidates: it is not representable, so NX must set under
every rounding mode. Distance to the lower candidate: 0.875
of an ulp (7/8). Distance to the upper candidate: 0.125 of an
ulp (1/8). The exact value is strictly closer to the upper
candidate.

- RNE (frm=0): the nearer candidate wins.
  Expect `0x3FF0000000000003`.
- RDN (frm=2): round toward -inf. The exact difference is
  positive, so the candidate toward -inf is the lower one.
  Expect `0x3FF0000000000002`.

The subtraction is inexact, so NX must set; no other flag can
move (both operands are finite positive normals, the result is
finite and nowhere near subnormal, so no invalid operand,
division by zero, overflow, or underflow is possible).

## Checks (all computed, none eyeballed)

Setup (7): `mtvec` took the handler address; `mtvec` is in
direct mode; `mie == 0` at boot; `misa` carries the F and D
extension bits; boot `mstatus` FS reads 0 (Off); after `csrs`,
FS reads 1 (Initial); after `csrs`, no non-FS bit changed and
SD reads 0.

Trial RDN (4): `fcsr` readback is exactly 0x40 after the write;
the result bits equal `0x3FF0000000000002`;
`fflags == 0x01` (NX set, no other flag); the frm field still
reads 2.

Trial RNE (6): `fcsr` readback is exactly 0x00 after the write;
the result bits equal `0x3FF0000000000003`;
`fflags == 0x01` (NX set, no other flag); the frm field still
reads 0; `q_rdn == q_rne - 1` as unsigned 64-bit values;
`q_rdn < q_rne`.

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

| trial | fcsr write | fcsr readback | result bits        | fcsr after | fflags | frm |
|-------|------------|---------------|--------------------|------------|--------|-----|
| sub (RDN) | 0x40     | 0x40          | 0x3ff0000000000002 | 0x41       | 0x1    | 2   |
| sub (RNE) | 0x00     | 0x00          | 0x3ff0000000000003 | 0x1        | 0x1    | 0   |

Neighbor relation: `q_rdn=0x3ff0000000000002`,
`q_rne=0x3ff0000000000003`, so `q_rdn == q_rne - 1` holds as
unsigned 64-bit values and `q_rdn < q_rne` holds. `fcsr`
restored to `0x0` with an exact readback. The end-of-run
`mstatus` readback: `0x8000000a00006000`, FS=3 (Dirty),
consistent with the FP writes having genuinely touched the FP
state. Trap record: count=0, mcause=0x0, mepc=0x0, mtval=0x0.
`checks=20 mismatches=0`, `checksum=0xa92c23f3ba9e7afc`,
`RESULT: PASS` on all 3 runs, QEMU exit code 0 on all 3 runs
(finisher shutdown path).

Every value above is deterministic across runs; nothing in the
measurement words depends on host timing (the only
timing-dependent read, `mtime`, is used solely as a UART-drain
timebase and is not part of the checksum).

The RDN/RNE split is the whole point: the only thing that
changed between the two trials was the frm field, written with
a single `csrw fcsr`, and the delivered bit pattern moved from
`0x3ff0000000000003` down to `0x3ff0000000000002`, exactly one
ulp toward -inf, with NX the only flag set in both trials.

## Limits of verification

- Emulator, not silicon: every number above is a property of
  QEMU 8.2.2's FP and CSR implementation on this host, not of
  physical RISC-V hardware. A real core's frm rounding is
  defined by the same spec text but was not measured here.
- The trap handler's mepc+4 skip is a safety net for this
  module's 4-byte FP/CSR instructions only; a correct run never
  exercises it (counter stayed 0).
- Scope: this module measures that the frm field selects RDN
  vs RNE rounding for `fsub.d` with rm=DYN in M-mode on QEMU.
  It does not test the other rounding modes (RNZ, RUP, RMM),
  single-precision operations, or rounding behavior across
  traps.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/boot.S -o src/boot.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/uart.c -o src/uart.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fcsr-frm-rounddn/frd_trap.S -o src/fcsr-frm-rounddn/frd_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fcsr-frm-rounddn/frd_main.c -o src/fcsr-frm-rounddn/frd_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o fcsr-frm-rounddn.elf src/boot.o src/uart.o src/fcsr-frm-rounddn/frd_trap.o src/fcsr-frm-rounddn/frd_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: fcsr-frm-rounddn.elf has a LOAD segment with RWX permissions
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
fcsr-frm-rounddn: frm=RDN rounds an inexact fsub.d toward -inf
setup: mtvec=0x800001c8 mie=0x0
setup: misa=0x80000000001411ad
setup: fcsr(boot)=0x0
sub RDN: fcsr-write=0x40 fcsr-readback=0x40 result=0x3ff0000000000002 fcsr=0x41 fflags=0x1 frm=2
sub RNE: fcsr-write=0x0 fcsr-readback=0x0 result=0x3ff0000000000003 fcsr=0x1 fflags=0x1 frm=0
rel: q_rdn=0x3ff0000000000002 q_rne=0x3ff0000000000003
end: fcsr(restored)=0x0
end: mstatus=0x8000000a00006000 FS=3
traps: count=0 mcause=0x0 mepc=0x0 mtval=0x0
checks=20 mismatches=0
checksum=0xa92c23f3ba9e7afc
RESULT: PASS
```

Runs 2 and 3 (run2.log, run3.log) are byte-identical to run 1.
QEMU exit code 0 on all 3 runs (finisher shutdown path).
