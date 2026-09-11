<!-- PROOF-HEADER
Checks: 19
Mismatches: 0
Checksum: 0xaaf26cfb83385742
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: a static rounding mode in the instruction overrides fcsr.frm (backlog item "riscv frm-dynamic-vs-static")

Backlog item "riscv frm-dynamic-vs-static": in M-mode on QEMU with
the FPU enabled, execute `fdiv.d` with a static RNE encoding under
frm=RDN and frm=RUP; both quotients must be the RNE result and frm
must read back untouched. 3 QEMU runs, quotient pairs published.

## What was built

`src/frm-dynamic-vs-static/`, a bare-metal M-mode binary sharing
only `src/boot.S` and the UART driver with the other demos. It
walks this sequence and checks every step in code:

1. Install the counting M-mode trap handler (`fvs_trap.S`) as a
   safety net; clear `mstatus.MIE`; assert `mie == 0` at boot.
2. Read `misa`; require the F (bit 5) and D (bit 3) extension bits
   (`fdiv.d` is a D-extension instruction).
3. Read the boot `mstatus`; require FS == 0 (Off); set FS to Dirty
   with `csrs mstatus, (3 << 13)` and read back FS == 3 before any
   FP write, since an FP instruction with FS == Off raises
   illegal-instruction.
4. Trial 1: `csrw fcsr, (RDN << 5)`, then `fdiv.d 1.0/3.0` with the
   STATIC RNE encoding (`fdiv.d f10, f10, f11, rne`, rm field 000).
   Require the quotient bits to equal the RNE result
   0x3FD5555555555555, require frm to still read back RDN, and
   require fflags to show NX only (the divide is inexact).
5. Trial 2: same static-RNE divide under frm=RUP; require the same
   quotient 0x3FD5555555555555 and frm still RUP. Require the two
   static quotients to agree with each other.
6. Dynamic controls (the same `fdiv.d` with the rm operand omitted,
   which the assembler encodes as rm=111): under frm=RDN require
   0x3FD5555555555555; under frm=RUP require
   0x3FD5555555555556. The RUP control must DIFFER from the static
   result under RUP: that divergence is what proves the divide
   really rounds per its rm input, so the static agreement in
   steps 4-5 is a genuine override and not a dead rounding path.
7. Require the trap counter to stay 0.

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
800003fe:  1ab50553   fdiv.d  fa0,fa0,fa1,rne   (static: rm bits 14:12 = 000)
80000504:  1ab57553   fdiv.d  fa0,fa0,fa1       (dynamic: rm bits 14:12 = 111)
```

The assembler accepts the static `rne` operand and encodes rm=000;
omitting the operand encodes rm=111 (dynamic). No `.word`
fallback was needed.

## A correction to the backlog item's expected values

The item as written expected the dynamic divide under frm=RDN to
yield 0x3FD5555555555554. That is not the IEEE 754 result for
1.0/3.0. The true quotient is 0x3FD5555555555555 plus one third of
an ulp, i.e. it lies strictly ABOVE the RNE double; rounding
toward negative infinity therefore lands on 0x3FD5555555555555,
the same bits as RNE (only RUP moves, to 0x3FD5555555555556).
The sibling module `frm-rounding-write` measured the same:
its run logs show `anchor RDN q=0x3fd5555555555555`. The module
asserts the correct value (dynamic RDN == 0x3FD5555555555555).
The override claim never depended on the wrong expectation: the
decisive measurement is under frm=RUP, where the static encoding
yields 0x3FD5555555555555 while the dynamic control yields
0x3FD5555555555556.

## Measured numbers (identical in all 3 runs)

- misa = 0x80000000001411ad (F and D bits set)
- mstatus FS after set = 3 (Dirty)
- static-RNE under frm=RDN: q = 0x3fd5555555555555, fcsr = 0x41
  (frm=2, fflags=0x1 = NX)
- static-RNE under frm=RUP: q = 0x3fd5555555555555, fcsr = 0x61
  (frm=3, fflags=0x1 = NX)
- dynamic under frm=RDN: q = 0x3fd5555555555555, fcsr = 0x41
- dynamic under frm=RUP: q = 0x3fd5555555555556, fcsr = 0x61
- traps: count=0
- checks=19 mismatches=0
- checksum (FNV-1a, 64-bit, over the 12 words misa, mstatus_boot,
  fs_set, q_s_rdn, fcsr_s_rdn, q_s_rup, fcsr_s_rup, q_d_rdn,
  fcsr_d_rdn, q_d_rup, fcsr_d_rup, trap_count, in that order) =
  0xaaf26cfb83385742
- RESULT: PASS in all 3 runs; the three run logs are
  byte-identical (`cmp` clean across run1/run2/run3).

The 19 checks: mtvec address, mtvec direct mode, mie==0, misa
F/D, boot FS==Off, FS==Dirty after set, the four quotient
equalities, the two static frm-untouched readbacks, the
static-quotient agreement, the dynamic-vs-static divergence under
RUP, the four NX-only fflags checks, and the zero trap count.

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
src/frm-dynamic-vs-static/fvs_trap.o
src/frm-dynamic-vs-static/fvs_main.o` against `link.ld`. No
compiler warnings (only the usual RWX LOAD-segment note from
`ld`). QEMU: `qemu-system-riscv64` 8.2.2, `-machine virt
-nographic -bios none -kernel frm-dynamic-vs-static.elf`, each
run under `timeout 20`.

```
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/boot.S -o src/boot.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/uart.c -o src/uart.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/frm-dynamic-vs-static/fvs_main.c -o src/frm-dynamic-vs-static/fvs_main.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/frm-dynamic-vs-static/fvs_trap.S -o src/frm-dynamic-vs-static/fvs_trap.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o frm-dynamic-vs-static.elf src/boot.o src/uart.o src/frm-dynamic-vs-static/fvs_trap.o src/frm-dynamic-vs-static/fvs_main.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/../lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: frm-dynamic-vs-static.elf has a LOAD segment with RWX permissions
built frm-dynamic-vs-static.elf
```

## Run outputs

`bench-logs/run1.log` (run2 and run3 are byte-identical):

```
frm-dynamic-vs-static: static rm in fdiv.d overrides fcsr.frm
setup: misa=0x80000000001411ad
setup: mstatus FS after set=3 (expect 3=Dirty)
static-RNE under frm=RDN: q=0x3fd5555555555555 fcsr=0x41 (frm=2 fflags=0x1)
static-RNE under frm=RUP: q=0x3fd5555555555555 fcsr=0x61 (frm=3 fflags=0x1)
dynamic under frm=RDN: q=0x3fd5555555555555 fcsr=0x41 (frm=2 fflags=0x1)
dynamic under frm=RUP: q=0x3fd5555555555556 fcsr=0x61 (frm=3 fflags=0x1)
traps: count=0 mcause=0x0 mepc=0x0 mtval=0x0
checks=19 mismatches=0
checksum=0xaaf26cfb83385742
RESULT: PASS
```

`bench-logs/run2.log`: byte-identical to run1.log (`cmp` clean),
exit 0.

`bench-logs/run3.log`: byte-identical to run1.log (`cmp` clean),
exit 0.

## What this establishes

The static rm=000 encoding of `fdiv.d` rounds with RNE even when
fcsr.frm says RDN or RUP: both static quotients are
0x3FD5555555555555 and frm reads back unchanged (RDN stays 2, RUP
stays 3) after each divide. The dynamic control under RUP yields
0x3FD5555555555556, a different quotient from the same
instruction/operands, which shows the rounding path is live and
the static agreement is a real override. fflags show NX only
after every divide, as 1.0/3.0 is inexact with no other exception.
