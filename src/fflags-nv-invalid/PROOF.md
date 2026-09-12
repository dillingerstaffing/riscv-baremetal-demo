<!-- PROOF-HEADER
Checks: 14
Mismatches: 0
Checksum: 0x1dbad5f6e633e840
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: fflags NV (invalid operation) accrual on fdiv.d 0.0/0.0 (backlog item "riscv fflags-nv-invalid")

Backlog item "riscv fflags-nv-invalid": in M-mode on QEMU, clear
`fflags`, run `fdiv.d` 0.0/0.0, and verify the NV bit is set with
NX/UF/OF/DZ all clear; publish the fcsr read triples across 3
runs. Distinct from the done `src/fflags-nx-inexact`,
`src/fflags-uf-underflow`, `src/fflags-of-overflow`, and
`src/fflags-dz-divide-by-zero` modules, which probe the NX, UF,
OF, and DZ accrued flags respectively: each probes a different
accrued flag.

## What was built

`src/fflags-nv-invalid/`, a bare-metal M-mode binary sharing only
`src/boot.S` and the UART driver with the other demos. It walks
this sequence and checks every step in code:

1. Installs a counting M-mode trap handler (`fnv_trap.S`, direct
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
6. Loads the bit pattern for +0.0 (`0x0000000000000000`) into
   integer registers, moves it into f2/f3 with `fmv.d.x`, and
   executes one volatile in-asm `fdiv.d f1, f2, f3, rne`. The
   volatile asm keeps the compiler from constant-folding or
   eliminating the divide. Requires the `fcsr` readback to be
   exactly `0x10`: NV set, NX/UF/OF/DZ all 0, frm still RNE.
7. Reads the quotient back with `fmv.x.d` and requires it to equal
   `0x7FF8000000000000` (the canonical quiet NaN, the 0.0/0.0
   result). This is a sanity anchor only; the verdict rests on
   the fflags checks.
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

Build integration: `Makefile` gains `fflags-nv-invalid.elf` and
`run-fflags-nv-invalid`; the module is in `all` and `clean`.

## Why the rounding mode is explicit

GAS assembles a bare `fdiv.d f1, f2, f3` with rm=DYN (dynamic
rounding): a sibling module's first build showed objdump dropping
the `,rne` field. The source spells out
`fdiv.d f1, f2, f3, rne` so the instruction in the binary is
unambiguous. The shipped binary's objdump line is:

```
800003ba:  1a3100d3   fdiv.d  ft1,ft2,ft3,rne
```

Field decode of `0x1a3100d3`: opcode `1010011`, rd=1 (f1),
rm=`000` (RNE), rs1=2 (f2), rs2=3 (f3), funct7=`0001101`. That
funct7 is FDIV.D per the ISA manual's OP-FP table (FADD.D is
`0000001`, FSUB.D `0000101`, FMUL.D `0001001`, FDIV.D `0001101`;
verified against the sibling fflags-dz-divide-by-zero module's
shipped encoding decode before shipping). One occurrence of this
encoding exists in the ELF, and no other FP arithmetic instruction
is present anywhere in the binary.

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
9. After `fdiv.d` 0.0/0.0, the fflags field (bits 4:0) reads
   exactly `0x10` (NV set, NX/UF/OF/DZ all 0).
10. After the `fdiv.d`, no non-fflags bit of `fcsr` moved
    (reserved bits and frm unchanged).
11. After the `fdiv.d`, frm still reads RNE (0).
12. The quotient bit pattern read back with `fmv.x.d` equals
    `0x7FF8000000000000` (canonical quiet NaN).
13. After the second `csrw fcsr, x0`, the `fcsr` readback is
    exactly `0x00`.
14. The trap counter is 0 (no trap fired).

## Measured read sequence

QEMU 8.2.2 (`qemu-system-riscv64 --version` reports
"QEMU emulator version 8.2.2"), `-machine virt`, 3 runs,
byte-identical (md5 `99b3273f6e4012a50875b99073ee7bbc` on all three
logs). `misa` readback: `0x80000000001411ad` (F and D bits set).
`mtvec` readback base `0x800001c8` with mode 0.

Run output (identical across all 3 runs):

```
fflags-nv-invalid: fdiv.d 0.0/0.0 must set fflags.NV
setup: mtvec=0x800001c8 mie=0x0
setup: misa=0x80000000001411ad
before: fcsr=0x0 fflags=0x0 frm=0
after : fcsr=0x10 fflags=0x10 frm=0
quotient bits=0x7ff8000000000000
cleared: fcsr=0x0 fflags=0x0 frm=0
traps: count=0 mcause=0x0 mepc=0x0 mtval=0x0
triple fcsr: before=0x0 after=0x10 cleared=0x0
checks=14 mismatches=0
checksum=0x1dbad5f6e633e840
RESULT: PASS
```

fcsr triple: `0x0` (cleared) -> `0x10` (after 0.0/0.0) ->
`0x0` (cleared again). NV is fflags bit 4, so `0x10` is NV set
with NX (bit 0), UF (bit 1), OF (bit 2), DZ (bit 3) all clear.

## Build log

Built with `riscv64-unknown-elf-gcc` 13.2.0
(Ubuntu `gcc-riscv64-unknown-elf` 13.2.0 package), flags
`-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie
-fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`,
linked with `link.ld` (boot.S first, `_start` at 0x80000000).
`make fflags-nv-invalid.elf` output:

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fflags-nv-invalid/fnv_trap.S -o src/fflags-nv-invalid/fnv_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fflags-nv-invalid/fnv_main.c -o src/fflags-nv-invalid/fnv_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o fflags-nv-invalid.elf src/boot.o src/uart.o src/fflags-nv-invalid/fnv_trap.o src/fflags-nv-invalid/fnv_main.o
ld: warning: fflags-nv-invalid.elf has a LOAD segment with RWX permissions
```

The RWX LOAD warning is the linker's standard note for this
repo's flat bare-metal link script; every module in the repo links
the same way.

QEMU invocation per run:

```
qemu-system-riscv64 -machine virt -nographic -bios none -kernel fflags-nv-invalid.elf
```

All three runs exited 0 (finisher path), proving the module took
the PASS branch.
