<!-- PROOF-HEADER
Checks: 50
Mismatches: 0
Checksum: 0xbd94fd8e55fa89f0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: mideleg-seip-clear

Checks: 50
Mismatches: 0
Checksum: 0xbd94fd8e55fa89f0
Environment: QEMU 8.2.2
Verdict: PASS

## What this proves

With SEIP already pending, clearing `mideleg` bit 9 moves the
next interrupt delivery from S-mode to M-mode, and re-setting
bit 9 moves delivery back to S-mode. Delegation selects which
privilege takes the trap; the trap record keeps the source's
cause code either way.

## Measured trap records (run 1 of 3; all 3 byte-identical)

Phase A (bit 9 set, S-mode enabled): exactly one S-mode trap.
`scause=0x8000000000000009`, `sepc=0x80000434` equal to the
in-assembly expected address, `sip` at handler entry `=0x200`
(SEIP). `m_traps` stayed 0.

Phase B (bit 9 cleared, M-mode enabled via `mie.SEIE` then
`mstatus.MIE`): exactly one M-mode trap.
`mcause=0x8000000000000009`, `mepc=0x800006fe` equal to the
interrupted instruction, `s_traps` stayed 1. Controls with the
enables off, and with `mie.SEIE` on but `mstatus.MIE` off,
delivered no trap.

Phase C (bit 9 re-set, S-mode enabled again): exactly one more
S-mode trap. `scause=0x8000000000000009`,
`sepc=0x80000c52` equal to the in-assembly expected address,
`sip` at entry `=0x200`. `m_traps` stayed 1.

Phase D (SEIP cleared): 2,000,000-cycle quiet window with
`s_traps=2`, `m_traps=1`, `ecalls=2` unchanged; `mideleg`
restored to the boot value `0x1444` and `mstatus` restored to the
boot value `0xa00000000`.

## How the checksum is built

The checksum folds the final trap-record arrays (`s_regs`,
`m_regs`), `mideleg`, `mstatus`, `sstatus`, and the three
counters. `m_regs[2]` holds the last M-mode trap cause, which is
the phase-B `0x8000000000000009` (no M-mode trap fired after
phase B), and `m_regs[7]` holds the ecall count `2` (one S-mode
`ecall` per S-mode phase, the mechanism that hands control back
to M-mode). All three runs produced `0xbd94fd8e55fa89f0`.

## Backlog corrections (two, stated honestly)

The backlog line said "pend STIP via a sip write" but named
`mcause=0xb` and `scause=0x8000000000000009`. First correction:
STIP is cause code 5, so the line conflated the timer and
external sources; the module pends SEIP with an M-mode
`csrs mip, 1<<9`, the same recipe the sibling module measured.
Second correction: the backlog's `mcause=0xb` is wrong for the
phase-B trap. The pending source is `mip.SEIP` (bit 9); clearing
its delegation moves the trap to M-mode without changing the
source's code, so the measured `mcause` is
`0x8000000000000009`. Code `0xb` would be the separate
machine-external source (`mip.MEIP`), which is never pending
here.

Two implementation findings from the bring-up, kept in the
module because they are part of the honest record. First, the
M-mode handler's `ecall` path must set `mstatus.MPP` to M-mode
before `mret`: the trap hardware sets MPP to S-mode, and `mret`
restores the privilege from MPP, so without the fix the phase
trampoline ran in S-mode and faulted. Second, the M-mode enable
for the SEIP source is `mie.SEIE` (bit 9), not `mie.MEIE`
(bit 11); `sie` is a restricted view of `mie`, so phase A's
S-mode `csrs sie, SEIE` also sets `mie` bit 9, which phase B and
phase D clear explicitly to keep each phase self-contained.

## Build log (genuine)

Toolchain: `riscv64-unknown-elf-gcc 13.2.0` (Ubuntu
gcc-riscv64-unknown-elf package), all objects rebuilt from
source for this log.

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/boot.S -o src/boot.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/uart.c -o src/uart.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/preempt/clint.c -o src/preempt/clint.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mideleg-seip-clear/seip_clear_trap.S -o src/mideleg-seip-clear/seip_clear_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mideleg-seip-clear/seip_clear_main.c -o src/mideleg-seip-clear/seip_clear_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o mideleg-seip-clear.elf src/boot.o src/uart.o src/preempt/clint.o src/mideleg-seip-clear/seip_clear_trap.o src/mideleg-seip-clear/seip_clear_main.o
ld: warning: mideleg-seip-clear.elf has a LOAD segment with RWX permissions
```

(The RWX warning is the standard bare-metal link notice; the
sibling modules report the same.)

## Run output (genuine, run 1 of 3)

Command (all three runs):

```
export LD_LIBRARY_PATH=~/workspace/qemu/usr/lib/x86_64-linux-gnu:~/workspace/qemu/lib/x86_64-linux-gnu
timeout 60 ~/workspace/qemu/usr/bin/qemu-system-riscv64 -machine virt -nographic -bios none -kernel mideleg-seip-clear.elf
```

Exits: 0, 0, 0. md5 of the three transcripts identical
(`7c3ab531e3879616aa929ab6fe282872` x3). Transcript:

```
mideleg-seip-clear: mideleg bit-9 clear moves a pended SEI from S-mode to M-mode
boot: mstatus=0xa00000000
trap: mtvec=0x800001e4
boot: mideleg=0x1444
mideleg: write=0x0 readback=0x1444
mideleg: write=0x200 readback=0x1644
mideleg: write=0x1644 readback=0x1644
medeleg: write=0x0 readback=0x0
trap: stvec=0x80000268
pend: mip-after-csrs=0x200
phaseA: sie=0x200
phaseA-control: s_traps=0 m_traps=0
phaseA-sei: spins=0 s_traps=1 scause=0x8000000000000009 sepc=0x80000434 expected=0x80000434 sip-at-entry=0x200
phaseB-mideleg-clear: write=0x1444 readback=0x1444
phaseB-pend: mip-after-csrs=0x200
phaseB-control1: s_traps=1 m_traps=0
phaseB-control2: s_traps=1 m_traps=0
phaseB-trap: spins=0 m_traps=1 mcause=0x8000000000000009 mepc=0x800006fe expected=0x800006fe s_traps=1
phaseB-mideleg-reset: write=0x1644 readback=0x1644
phaseC: sie=0x200
phaseC-sei: spins=0 s_traps=2 scause=0x8000000000000009 sepc=0x80000c52 expected=0x80000c52 sip-at-entry=0x200 m_traps=1
phaseD-pend: mip-after-csrs=0x200
quiet: s_traps=2 m_traps=1 ecalls=2
restore: mideleg=0x1444
restore: mstatus=0xa00000000 boot=0xa00000000
record checksum=0xbd94fd8e55fa89f0
RESULT: PASS (checks=50)
```

FAIL would print `RESULT: FAIL (checks=50 fails=N)` and park
the hart in a `wfi` loop without touching the finisher (exit
124 under `timeout`); it did not happen in any of the three
runs.
