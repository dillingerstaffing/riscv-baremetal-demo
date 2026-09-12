<!-- PROOF-HEADER
Checks: 17
Mismatches: 0
Checksum: 0x6865065142aba3c7
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: fcsr fflags/frm field independence (backlog item "riscv fcsr-field-independence")

Backlog item "riscv fcsr-field-independence": in M-mode on QEMU,
verify that the two fields of the `fcsr` CSR, fflags (bits 4:0)
and frm (bits 7:5), are independent: a CSR read-modify-write
(`csrs`/`csrc`) on one field must not disturb the other. Publish
the fcsr readback sequence across 3 runs. Distinct from the done
`src/frm-rounding-write` item, which measures frm write/readback
for all five rounding modes, and from the done fflags accrual
items, which measure flag accrual; this module measures the
cross-field independence of the two operations.

## What was built

`src/fcsr-field-independence/`, a bare-metal M-mode binary
sharing only `src/boot.S` and the UART driver with the other
demos. It walks this sequence and checks every step in code:

1. Installs a counting M-mode trap handler (`ffi_trap.S`, direct
   mode, `mscratch` pointing at the trap record) as a safety net,
   clears `mstatus.MIE`, and asserts `mie == 0` at boot so no
   interrupt can fire.
2. Reads `misa` and requires the F and D extension bits, because
   the sequence executes `fdiv.d`, a D-extension instruction.
3. Reads the boot `mstatus` and requires the FS field (bits 14:13)
   to read 0 (Off).
4. Sets FS to Initial (1) with `csrs mstatus`; requires the
   readback FS to be 1 with SD 0 and no other bit changed vs the
   baseline. This step is required before any FP use: with
   FS == Off an FP instruction raises illegal-instruction.
5. Step 1: clears the FP state with `csrw fcsr, x0` (fflags=0,
   frm=RNE); requires the `fcsr` readback to be exactly `0x00`.
6. Step 2: executes one inexact double divide 1.0/3.0 as a real
   volatile in-asm `fdiv.d f1, f2, f3, rne` (operands moved from
   the 1.0/3.0 bit patterns with `fmv.d.x`, so the NX flag is
   accrued by genuine hardware behavior, not set by hand);
   requires the `fcsr` readback to be exactly `0x01`, i.e. NX
   set, no other flag moved, frm still RNE.
7. Step 3: `csrs fcsr, (3<<5)` sets frm to RUP; requires the
   readback to be exactly `0x61`, i.e. the accrued NX flag
   preserved and frm == 3. This is the core
   one-field-must-not-disturb-the-other assertion: the flags
   field had to survive a write aimed only at frm.
8. Step 4: `csrc fcsr, 0x1f` clears the fflags field; requires
   the readback to be exactly `0x60`, i.e. frm preserved at RUP
   while every flag bit went back to 0. The reverse direction:
   the frm field survived a write aimed only at fflags.
9. Step 5: `csrw fcsr, x0` again; requires the readback to be
   exactly `0x00`.
10. Requires the trap counter to be 0.

The five fcsr readbacks are printed with their fflags/frm
decodes, a 64-bit FNV-1a checksum over the eight logged
measurement words is printed, and `RESULT: PASS` prints only
when every check holds. On PASS the module writes the virt
test-device finisher word `0x5555` at `0x100000`, which shuts the
machine down (QEMU exits 0); on FAIL it parks the hart in a
`wfi` loop without touching the finisher, so under the harness's
`timeout` a FAIL is observable as exit status 124 as well as the
`RESULT: FAIL` line.

The FP instructions assemble under in-asm `.option arch, +d`
because the module builds with `-march=rv64imac_zicsr` (no F/D);
the compiler can never allocate f1/f2/f3 under that march, so no
register clobber is needed. The divide's operand words move
through integer registers only, so no host floating-point
arithmetic is involved in feeding the operation. `src/boot.S` is
first in the link order so `_start` lands at 0x80000000.

Build integration: `Makefile` gains `fcsr-field-independence.elf`
and `run-fcsr-field-independence`; the module is in `all` and
`clean`.

## The rounding mode is explicit in the shipped binary

GAS assembles a bare `fdiv.d f1, f2, f3` with rm=DYN (dynamic
rounding); the source writes `fdiv.d f1, f2, f3, rne` so the
instruction in the binary is unambiguous. The shipped binary's
objdump line is:

```
800003c2:  1a3100d3   fdiv.d  ft1,ft2,ft3,rne
```

Field decode of `0x1a3100d3`: opcode `1010011`, rd=1 (f1),
rm=`000` (RNE), rs1=2 (f2), rs2=3 (f3), funct7=`0001101`
(FDIV.D). The binary also carries the expected
`csrs fcsr, a5` at `0x800003e2` and `csrc fcsr, a5` at
`0x80000402`.

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
9. After the inexact `fdiv.d`, the `fcsr` readback is exactly
   `0x01` (NX set, frm still RNE).
10. After `csrs fcsr, (3<<5)`, the `fcsr` readback is exactly
    `0x61`.
11. After that `csrs`, the fflags field still reads `0x01`
    (the field write did not touch it).
12. After that `csrs`, frm reads 3 (RUP).
13. After `csrc fcsr, 0x1f`, the `fcsr` readback is exactly
    `0x60`.
14. After that `csrc`, no fflags bit survived (field reads 0).
15. After that `csrc`, frm still reads 3 (RUP).
16. After the second `csrw fcsr, x0`, the `fcsr` readback is
    exactly `0x00`.
17. The trap counter is 0 (no trap fired).

## Measured read sequence

QEMU 8.2.2 (`qemu-system-riscv64 --version` reports
"QEMU emulator version 8.2.2"), `-machine virt`, 3 runs,
byte-identical. `misa` readback: `0x80000000001411ad` (F and D
bits set). `mtvec` readback: `0x800001c8` direct mode; `mie`
readback: `0x0`.

| step                        | `fcsr` readback | fflags | frm |
|-----------------------------|-----------------|--------|-----|
| 1 (cleared)                 | 0x0             | 0x0    | 0   |
| 2 (after fdiv.d, NX set)    | 0x1             | 0x1    | 0   |
| 3 (csrs frm=RUP)            | 0x61            | 0x1    | 3   |
| 4 (csrc clear fflags)       | 0x60            | 0x0    | 3   |
| 5 (cleared)                 | 0x0             | 0x0    | 0   |

Read sequence (s1..s5): `0x0, 0x1, 0x61, 0x60, 0x0`. Trap
record: count=0, mcause=0x0, mepc=0x0, mtval=0x0. `checks=17
mismatches=0`, `checksum=0x6865065142aba3c7`,
`RESULT: PASS` on all 3 runs, QEMU exit code 0 on all 3 runs.

Every value above is deterministic across runs; nothing in the
measurement words depends on host timing (the only
timing-dependent read, `mtime`, is used solely as a UART-drain
timebase and is not part of the checksum).

## Limits of verification

- Emulator, not silicon: every number above is a property of QEMU
  8.2.2's FP and CSR implementation on this host, not of physical
  RISC-V hardware. A real core's fcsr field behavior is defined
  by the same spec text but was not measured here.
- The trap handler's mepc+4 skip is a safety net for this
  module's 4-byte instructions only; a correct run never
  exercises it (counter stayed 0).
- Scope: this module measures field independence of `fcsr`
  under `csrw`/`csrs`/`csrc` in M-mode on QEMU. It does not test
  the other flag-accrual cases, single-precision operations, or
  field behavior across traps.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fcsr-field-independence/ffi_trap.S -o src/fcsr-field-independence/ffi_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fcsr-field-independence/ffi_main.c -o src/fcsr-field-independence/ffi_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o fcsr-field-independence.elf src/boot.o src/uart.o src/fcsr-field-independence/ffi_trap.o src/fcsr-field-independence/ffi_main.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/../lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: fcsr-field-independence.elf has a LOAD segment with RWX permissions
```

The RWX warning comes from the shared `link.ld` (used by every
module in this repo) and is present in the other modules' build
logs too; no new warning was introduced by this module.

## Run logs (QEMU 8.2.2, all 3 runs)

Run 1 (run1.log):
```
fcsr-field-independence: csrs/csrc on one fcsr field must not disturb the other
setup: mtvec=0x800001c8 mie=0x0
setup: misa=0x80000000001411ad
step1: fcsr=0x0 fflags=0x0 frm=0
step2: fcsr=0x1 fflags=0x1 frm=0
step3: fcsr=0x61 fflags=0x1 frm=3
step4: fcsr=0x60 fflags=0x0 frm=3
step5: fcsr=0x0 fflags=0x0 frm=0
traps: count=0 mcause=0x0 mepc=0x0 mtval=0x0
sequence fcsr: s1=0x0 s2=0x1 s3=0x61 s4=0x60 s5=0x0
checks=17 mismatches=0
checksum=0x6865065142aba3c7
RESULT: PASS
```

Runs 2 and 3 (run2.log, run3.log) are byte-identical to run 1.
QEMU exit code 0 on all 3 runs (finisher shutdown path).
