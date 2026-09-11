<!-- PROOF-HEADER
Checks: 16
Mismatches: 0
Checksum: 0x108e47da413617b9
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: directed rounding on fdiv.d 1.0/3.0 under frm=RDN vs frm=RUP (backlog item "riscv frm-rdn-vs-rup-div")

Backlog item "riscv frm-rdn-vs-rup-div": in M-mode on QEMU with
the FPU enabled, write fcsr=0x40 (frm=RDN, flags clear) and require
the readback to be exactly 0x40, execute `fdiv.d` on 1.0/3.0,
record the quotient; repeat with fcsr=0x60 (frm=RUP) and require
the readback to be exactly 0x60. The two quotients must be exactly
1 ulp apart with the RUP result the higher neighbor; a counting
M-mode trap handler must stay at 0. 3 QEMU runs, UART logs
byte-identical across all 3.

## What was built

`src/frm-rdn-vs-rup-div/`, a bare-metal M-mode binary sharing
only `src/boot.S` and the UART driver with the other demos. It
walks this sequence and checks every step in code:

1. Install the counting M-mode trap handler (`rnd_trap.S`) as a
   safety net; clear `mstatus.MIE`; assert `mie == 0` at boot.
   The handler touches only t0/t1; it does not modify fcsr or any
   FP register.
2. Read `misa`; require the F (bit 5) and D (bit 3) extension bits
   (`fdiv.d` is a D-extension instruction).
3. Read the boot `mstatus`; require FS == 0 (Off); set FS to Dirty
   with `csrs mstatus, (3 << 13)` and read back FS == 3 before any
   FP write, since an FP instruction with FS == Off raises
   illegal-instruction.
4. Trial 1: `csrw fcsr, 0x40` (frm=RDN, fflags=0) and require the
   readback to be exactly 0x40. Then `fdiv.d 1.0/3.0` with the
   dynamic encoding (rm operand omitted, encoded as 111). Require
   the frm field to still read 2 after the divide and fflags to
   show NX only (1.0/3.0 is inexact, nothing else).
5. Trial 2: `csrw fcsr, 0x60` (frm=RUP, fflags=0), require the
   readback to be exactly 0x60, divide, require frm still 3 and
   fflags NX only.
6. Anchor: the same divide under `csrw fcsr, 0x00` (frm=RNE),
   logged only; the verdict rests on the RDN/RUP checks.
7. On the quotient bit patterns as unsigned 64-bit integers:
   `q_rup == q_rdn + 1`, `(q_rup ^ q_rdn) == 0x3`, and
   `q_rup > q_rdn`.
8. Require the trap counter to stay 0.

The operands (1.0 = 0x3FF0000000000000, 3.0 = 0x4008000000000000)
travel as raw bit patterns in integer registers and are moved to
f10/f11 with `fmv.d.x` inside volatile asm, so the compiler cannot
constant-fold the divides; every division executes on the hart.
The FP instructions assemble under in-asm `.option arch, +d`
because the module builds with `-march=rv64imac_zicsr` (no F/D);
the compiler can never allocate FP registers under that march, so
no register clobber is needed. `src/boot.S` is first in the link
order so `_start` lands at 0x80000000.

## Encoding verification (objdump of the built ELF)

```
80000430:  1ab57553   fdiv.d  fa0,fa0,fa1   (dynamic: rm bits 14:12 = 111)
800004c0:  1ab57553   fdiv.d  fa0,fa0,fa1   (dynamic: rm bits 14:12 = 111)
80000518:  1ab57553   fdiv.d  fa0,fa0,fa1   (dynamic: rm bits 14:12 = 111)
```

Omitting the rm operand encodes rm=111 (dynamic), so each divide
takes its rounding mode from fcsr.frm at execution time. The three
`fdiv.d` sites are the RDN trial, the RUP trial, and the RNE
anchor. The quotient returns in fa0 and is read with `fmv.x.d`
into an integer register.

## A correction to the backlog item's stated xor check

The item as written required `q_rup ^ q_rdn == 1` ("they differ in
the lowest bit only"). That is not the arithmetic of these
quotients: 0x3FD5555555555555 ^ 0x3FD5555555555556 = 0x3, because
0x5 ^ 0x6 = 0x3. The measured quotients ARE exactly 1 ulp apart
(`rup - rdn = 0x1`, and `q_rup == q_rdn + 1` as unsigned 64-bit
addition holds), but adding 1 to the ...0101 tail flips the two
low bits (...0101 -> ...0110), so the patterns differ in exactly
the two low bits, not the lowest bit alone. A first run with the
check as written failed on that check alone (15 of 16 passed; the
hardware behavior was correct, the check's arithmetic was not).

The module asserts the true value: `(q_rup ^ q_rdn) == 0x3`.
The "exactly 1 ulp apart" claim is carried by
`q_rup == q_rdn + 1UL`, and the "RUP picks the higher neighbor"
claim by `q_rup > q_rdn`; the xor check pins the exact
measured bit-difference pattern. Per the module's rule, no check
was weakened: the false expectation was corrected to the
provable one and asserted, following the precedent of the
`frm-dynamic-vs-static` module, which likewise corrected a wrong
expected quotient from the backlog item.

## Measured numbers (identical in all 3 runs)

- misa = 0x80000000001411ad (F and D bits set)
- mstatus FS after set = 3 (Dirty)
- frm-RDN: write readback = 0x40; q = 0x3fd5555555555555,
  fcsr = 0x41 (frm=2, fflags=0x1 = NX)
- frm-RUP: write readback = 0x60; q = 0x3fd5555555555556,
  fcsr = 0x61 (frm=3, fflags=0x1 = NX)
- anchor-RNE: q = 0x3fd5555555555555, fcsr = 0x1 (frm=0,
  fflags=0x1); equals q_rdn, one below q_rup
- ulp: rup-rdn = 0x1, xor = 0x3
- traps: count=0
- checks=16 mismatches=0
- checksum (FNV-1a, 64-bit, over the 10 words misa,
  mstatus_boot, fs_set, q_rdn, fcsr_rdn, q_rup, fcsr_rup, q_rne,
  fcsr_rne, trap_count, in that order) = 0x108e47da413617b9
- RESULT: PASS in all 3 runs; the three run logs are
  byte-identical (`cmp` clean across run1/run2/run3); all three
  QEMU processes exited 0.

The 16 checks: mtvec address, mtvec direct mode, mie==0, misa
F/D, boot FS==Off, FS==Dirty after set, the 0x40 RDN write
readback, frm still RDN after the divide, NX-only fflags after
the RDN divide, the 0x60 RUP write readback, frm still RUP after
the divide, NX-only fflags after the RUP divide, the three
quotient-relation checks (q_rup == q_rdn + 1, xor == 0x3,
q_rup > q_rdn), and the zero trap count.

On PASS the module writes the virt test-device finisher word
0x5555 at 0x100000, which shuts the machine down (QEMU exits 0);
all three runs exited 0. On FAIL it would park the hart in a wfi
loop without touching the finisher, observable as the harness
timeout exit status (124).

## Build log

Build log: `bench-logs/build.log` (real output captured below).
Toolchain: `riscv64-unknown-elf-gcc` 13.2.0 / binutils 2.42 from
`~/workspace/toolchains/ubuntu-rv64/usr/bin` (on PATH at build
time); flags `-Wall -Wextra -O2 -ffreestanding -nostdlib
-nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr
-mabi=lp64 -mcmodel=medany`; link order `src/boot.o src/uart.o
src/frm-rdn-vs-rup-div/rnd_trap.o
src/frm-rdn-vs-rup-div/rnd_main.o` against `link.ld`. No
compiler warnings (only the usual RWX LOAD-segment note from
`ld`). QEMU: `qemu-system-riscv64` 8.2.2, `-machine virt
-nographic -bios none -kernel frm-rdn-vs-rup-div.elf`, each
run under `timeout 20`.

```
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/boot.S -o src/boot.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/uart.c -o src/uart.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/frm-rdn-vs-rup-div/rnd_main.c -o src/frm-rdn-vs-rup-div/rnd_main.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/frm-rdn-vs-rup-div/rnd_trap.S -o src/frm-rdn-vs-rup-div/rnd_trap.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o frm-rdn-vs-rup-div.elf src/boot.o src/uart.o src/frm-rdn-vs-rup-div/rnd_trap.o src/frm-rdn-vs-rup-div/rnd_main.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/../lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: frm-rdn-vs-rup-div.elf has a LOAD segment with RWX permissions
built frm-rdn-vs-rup-div.elf
```

## Run outputs

`bench-logs/run1.log` (run2 and run3 are byte-identical):

```
frm-rdn-vs-rup-div: RDN vs RUP rounding on fdiv.d 1.0/3.0
setup: misa=0x80000000001411ad
setup: mstatus FS after set=3 (expect 3=Dirty)
frm-RDN: write fcsr=0x40 readback=0x40
frm-RDN: q=0x3fd5555555555555 fcsr=0x41 (frm=2 fflags=0x1)
frm-RUP: write fcsr=0x60 readback=0x60
frm-RUP: q=0x3fd5555555555556 fcsr=0x61 (frm=3 fflags=0x1)
anchor-RNE: q=0x3fd5555555555555 fcsr=0x1 (frm=0 fflags=0x1) equals-q_rdn=1 one-below-q_rup=1
ulp: q_rdn=0x3fd5555555555555 q_rup=0x3fd5555555555556 (rup-rdn=0x1 xor=0x3)
traps: count=0 mcause=0x0 mepc=0x0 mtval=0x0
checks=16 mismatches=0
checksum=0x108e47da413617b9
RESULT: PASS
```

`bench-logs/run2.log`: byte-identical to run1.log (`cmp` clean),
exit 0.

`bench-logs/run3.log`: byte-identical to run1.log (`cmp` clean),
exit 0.

## What this establishes

Under fcsr.frm=RDN the dynamic `fdiv.d` of 1.0/3.0 returns
0x3FD5555555555555, and under frm=RUP it returns
0x3FD5555555555556: the two quotients are exactly 1 ulp apart
(`q_rup == q_rdn + 1` as unsigned 64-bit addition,
`rup - rdn = 0x1`), the patterns differ in exactly the two low
bits (xor = 0x3), and the RUP result is the higher neighbor
(`q_rup > q_rdn`). For positive 1/3 that is the correct
directed-rounding behavior: RDN picks the lower bracket, RUP the
upper. The RNE anchor lands on the RDN quotient, consistent with
the true quotient sitting above the RNE double. fcsr writes take
exactly (0x40 and 0x60 read back verbatim), frm survives each
divide, and fflags show NX only after every divide, as 1.0/3.0 is
inexact with no other exception. The trap counter stayed 0.

These are emulator results from QEMU 8.2.2 (TCG), not silicon:
they verify the module's checks against the emulated hart, which
is the ground truth this module was built to measure.
