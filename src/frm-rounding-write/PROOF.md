<!-- PROOF-HEADER
Checks: 17
Mismatches: 0
Checksum: 0x44951e60b5903b3c
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: every fcsr.frm rounding mode writes and reads back (backlog item "riscv frm-rounding-write")

Backlog item "riscv frm-rounding-write": in M-mode on QEMU, write
each fcsr.frm mode and require it to read back; publish the
written-vs-readback frm pairs across 3 runs.

## What was built

`src/frm-rounding-write/`, a bare-metal M-mode binary sharing only
`src/boot.S` and the UART driver with the other demos. It walks this
sequence and checks every step in code:

1. Install the counting M-mode trap handler (`frw_trap.S`) as a
   safety net; clear `mstatus.MIE`; assert `mie == 0` at boot.
2. Read `misa`; require the F (bit 5) and D (bit 3) extension bits.
3. Read the boot `mstatus`; require FS == 0 (Off); set FS to Initial
   (1) with `csrs` before any FP write, since an FP instruction with
   FS == Off raises illegal-instruction.
4. For each of the five rounding modes (RNE=0, RTZ=1, RDN=2, RUP=3,
   RMM=4): write `fcsr = (mode << 5)` with `csrw fcsr` (fflags
   written 0), read `fcsr` back, and require the frm field
   (bits 7:5) to equal the written mode and the fflags field
   (bits 4:0) to still be 0.
5. Sanity anchor, logged only (the verdict rests on step 4): under
   RDN then RUP, execute `fdiv.d 1.0/3.0` with operands loaded via
   `fmv.d.x` from integer registers holding the exact double bit
   patterns 0x3FF0000000000000 and 0x4008000000000000 in volatile
   asm (nothing constant-folded), and log the quotient bit
   patterns. Expected: RDN gives 0x3FD5555555555555, RUP gives
   0x3FD5555555555556, proving the written mode steers hardware
   rounding. The divide is inexact, so fflags gain NX here; step 6
   clears them.
6. Restore frm=RNE (`csrw fcsr, 0`); require the full `fcsr` word to
   read back 0x00.
7. Require the trap counter to stay 0.

The written-vs-readback frm pairs are printed every run; a 64-bit
FNV-1a checksum over the eleven logged measurement words (boot
mstatus, misa, the five fcsr readbacks, the two anchor quotients,
the restored fcsr, the trap count) is printed, and `RESULT: PASS`
prints only when every check holds. On PASS the module writes the
virt test-device finisher word `0x5555` at `0x100000`, which shuts
the machine down (QEMU exits 0); on FAIL it parks the hart in a
`wfi` loop without touching the finisher, so under the harness's
`timeout` a FAIL is observable as exit status 124 as well as the
`RESULT: FAIL` line.

The FP instructions assemble under in-asm `.option arch, +d`
because the module builds with `-march=rv64imac_zicsr` (no F/D);
the compiler can never allocate FP registers under that march, so
no register clobber is needed. `src/boot.S` is first in the link
order so `_start` lands at 0x80000000.

Build integration: `Makefile` gains `frm-rounding-write.elf` and
`run-frm-rounding-write`; the module is in `all` and `clean`.

Scope note: this module covers only the frm field of fcsr.
`mstatus.FS` is covered by `src/sstatus-fs-dirty`; the fflags field
is the sibling backlog item ("riscv fflags-nx-inexact").

## Spec grounding

- `fcsr` layout (fflags bits 4:0, frm bits 7:5) and the five
  rounding-mode encodings: RISC-V Unprivileged ISA, Floating-Point
  chapter (F/D extensions), sections 11.3 (Floating-Point Control
  and Status Register) and 11.4 (Rounding Modes).
- frm is a WARL field: a legal write reads back as written; the
  five encodings 0..4 are the legal set.
- Verification environment: QEMU 8.2.2 (`qemu-system-riscv64
  --version` reports 8.2.2), machine virt, `-bios none`.

## Build and runs

Toolchain: riscv64-unknown-elf-gcc 13.2.0
(`~/workspace/toolchains/ubuntu-rv64/usr/bin` first on PATH).

Build log: `bench-logs/build.log` (real output captured below).

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/frm-rounding-write/frw_trap.S -o src/frm-rounding-write/frw_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/frm-rounding-write/frw_main.c -o src/frm-rounding-write/frw_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o frm-rounding-write.elf src/boot.o src/uart.o src/frm-rounding-write/frw_trap.o src/frm-rounding-write/frw_main.o
ld: warning: frm-rounding-write.elf has a LOAD segment with RWX permissions
```

(The RWX warning is emitted for every module built with this
link script, including the template module; the load addresses are
still correct and the binary boots.)

Run logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs are byte-identical (md5 27c41234d9ec58a0b2ab7e4ab2197bf1 for
each). Run 1 output:

```
frm-rounding-write: every fcsr.frm mode writes and reads back
setup: misa=0x80000000001411ad
mode RNE: written frm=0 readback fcsr=0x0 frm=0 fflags=0
mode RTZ: written frm=1 readback fcsr=0x20 frm=1 fflags=0
mode RDN: written frm=2 readback fcsr=0x40 frm=2 fflags=0
mode RUP: written frm=3 readback fcsr=0x60 frm=3 fflags=0
mode RMM: written frm=4 readback fcsr=0x80 frm=4 fflags=0
anchor RDN q=0x3fd5555555555555 expected=0x3fd5555555555555 ok=1
anchor RUP q=0x3fd5555555555556 expected=0x3fd5555555555556 ok=1
restored fcsr=0x0
traps: count=0 mcause=0x0 mepc=0x0 mtval=0x0
checks=17 mismatches=0
checksum=0x44951e60b5903b3c
RESULT: PASS
```

## Result table

| quantity | value |
|---|---|
| written vs readback frm pairs (3 runs) | RNE 0/0, RTZ 1/1, RDN 2/2, RUP 3/3, RMM 4/4 (identical all runs) |
| fflags after each fcsr write | 0 every time (untouched) |
| anchor quotients | RDN 0x3FD5555555555555, RUP 0x3FD5555555555556 (both matched) |
| restored fcsr | 0x00 |
| traps | 0 |
| checks / mismatches | 17 / 0 |
| checksum (FNV-1a, 11 words) | 0x44951e60b5903b3c |
| environment | QEMU 8.2.2 |
| verdict | PASS |

What was verified: the frm field of fcsr is a writable WARL
field: each of the five legal rounding modes written via `csrw
fcsr` read back exactly, and the write left fflags at 0. The
anchor showed the written mode steers real hardware rounding
(1.0/3.0 rounds down under RDN and up under RUP). Restoring
frm=RNE returned the full fcsr word to 0x00.
