<!-- PROOF-HEADER
Checks: 19
Mismatches: 0
Checksum: 0x580db705e095b710
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: the M-mode virtual-memory trap switch mstatus.TVM

## What was built

`src/mstatus-tvm-trap/`: a bare-metal RISC-V program that verifies
the TVM (Trap Virtual Memory) bit, bit 20 of mstatus, gates S-mode
access to virtual-memory control on the QEMU `virt` board. Three
files, sharing only `src/boot.S` and `src/uart.c` with the other
demos, wired into the Makefile as `mstatus-tvm-trap.elf` (boot.o
first in link order).

- `tvt_trap.S`: the M-mode trap entry (saves all registers on the
  current stack, calls the C handler, restores, mret), the two
  M-mode phase drivers (`tvt_phase1`, `tvt_phase2`), and the shared
  S-mode probe pad. Each phase driver arms sepc with the pad,
  sets sstatus SPP=1 (leaving TVM exactly as M-mode established it:
  set in phase 1, clear in phase 2), records its resume label address
  with an in-asm label (never hardcoded) plus the ecall expectation
  flag, then sret. The pad writes 0 (the Bare MODE value, so
  translation is never enabled) to satp, reads satp back, executes
  `sfence.vma`, then issues one ecall back to M-mode. The satp write
  site, the satp read site, and the fence site are global labels, and
  every probe instruction is emitted under `.option norvc` (4 bytes
  each), so the handler's fixed mepc+4 skip always resumes at the
  next probe instruction.
- `tvt_main.c`: the M-mode driver. Installs the vector, opens a PMP
  NAPOT region over the whole address space for S-mode (with no PMP
  entry, lower-privilege fetches fault), keeps medeleg/mideleg at
  zero so every trap lands in M-mode, keeps mie and mstatus.MIE
  clear so no interrupt can pollute the trap counts, then runs the
  two phases and 19 checks. The M-mode handler records
  cause/epc/mtval per trap, advances mepc by 4 past the expected
  synchronous probe traps, and redirects the phase-end ecall back to
  the recorded M-mode resume label with MPP restored to M-mode.

## The claim, stated honestly

The privileged architecture (section 3.1.6) defines TVM as the
M-mode switch for virtual-memory control: when TVM=1, S-mode
attempts to access satp or execute `sfence.vma` raise an
illegal-instruction exception. sstatus carries no TVM bit, so
S-mode cannot observe or change the switch, only feel its effect.

Premise correction, measured on the hart: the backlog gloss said
"writes to satp trap". The first QEMU run showed the S-mode satp
READ trapping as well (mcause=0x2, mepc exactly at the `csrr`
site), and the spec text covers "attempts to read or write the
satp CSR". The module therefore proves the true rule rather than
the gloss: with TVM=1 the pad takes three illegal-instruction
traps (satp write site, satp read site, sfence.vma site); with
TVM=0 the identical pad takes none.

This module is distinct from its siblings: `mstatus-tw-trap` tests
the TW (timeout-wait) bit, a different mstatus switch trapping a
different operation; `satp-mode-warl` probes the satp MODE field's
WARL legalization but never traps on the access itself. This is the
access trap: the operation is legal in S-mode with the switch clear
and illegal with it set.

## Results (identical across all 3 runs)

| # | Check | Result |
|---|-------|--------|
| 1 | TVM clear at boot (mstatus=0xa00000000) | pass |
| 2 | TVM set via csrs reads back set (0xa00100000) | pass |
| 3 | setting TVM disturbed no other mstatus bit | pass |
| 4 | phase-1 trap count == 4 (3 probes + ecall) | pass |
| 5 | trap 1 cause == 0x2 (illegal instruction) | pass |
| 6 | trap 1 mepc == satp write site (0x800002cc) | pass |
| 7 | trap 2 cause == 0x2 | pass |
| 8 | trap 2 mepc == satp read site (0x800002d0) | pass |
| 9 | trap 3 cause == 0x2 | pass |
| 10 | trap 3 mepc == sfence.vma site (0x800002e0) | pass |
| 11 | trap 4 cause == 0x9 (S-mode ecall) | pass |
| 12 | trap 4 mepc inside the pad range | pass |
| 13 | satp reads 0 from M-mode after phase 1 (trapped write had no effect) | pass |
| 14 | TVM clear via csrc reads back clear | pass |
| 15 | phase-2 new trap count == 1 (ecall only) | pass |
| 16 | phase-2 trap cause == 0x9 | pass |
| 17 | phase-2 ecall mepc inside the pad range | pass |
| 18 | satp readback == 0 (Bare write stuck) | pass |
| 19 | boot mstatus restored bit-for-bit | pass |

19 checks, 0 mismatches, FNV-1a checksum 0x580db705e095b710 over
the deterministic verdict values, byte-identical across 3 QEMU
8.2.2 runs. QEMU exited 0 on all three runs (the virt test-device
finisher fired, which the driver only touches on the all-pass
path).

## Limits of verification

- The probe covers the QEMU 8.2.2 `virt` implementation of TVM.
  The trap-on-read finding matches the privileged spec's "read or
  write" wording, but a physical hart could legalize the read side
  differently; the module documents what this hart does.
- Only the Bare satp value (0) is written, so no page-table walk is
  ever enabled; the module proves the access trap, not any
  translation behavior.
- Single hart, interrupts fully disarmed; the trap counts are only
  meaningful because nothing else can trap.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mstatus-tvm-trap/tvt_trap.S -o src/mstatus-tvm-trap/tvt_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mstatus-tvm-trap/tvt_main.c -o src/mstatus-tvm-trap/tvt_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o mstatus-tvm-trap.elf src/boot.o src/uart.o src/mstatus-tvm-trap/tvt_trap.o src/mstatus-tvm-trap/tvt_main.o
ld: warning: mstatus-tvm-trap.elf has a LOAD segment with RWX permissions
```

(Toolchain: Ubuntu riscv64-unknown-elf-gcc 13.2.0. The RWX warning
comes from the repo's shared link.ld and appears for every module.)

Probe-site encoding check (objdump of the built ELF), confirming
each probe instruction is 4 bytes so the mepc+4 skip is exact:

```
800002cc:  18001073    csrw  satp,zero     (tvt_satp_site)
800002d0:  18002373    csrr  t1,satp       (tvt_satp_read_site)
800002d4:  00001297    auipc t0,0x1        (la tvt_satp_rb)
800002d8:  ddc28293    addi  t0,t0,-548
800002dc:  0062b023    sd    t1,0(t0)
800002e0:  12000073    sfence.vma          (tvt_fence_site)
800002e4:  00000073    ecall
```

## Full console output (run 1 of 3; runs 2 and 3 byte-identical)

```
mstatus-tvm-trap: M-mode virtual-memory trap switch mstatus.TVM
boot: mstatus=0xa00000000
pad: satp-site=0x800002cc satp-read-site=0x800002d0 fence-site=0x800002e0
phase1: mstatus-after-set=0xa00100000
phase1: traps=4 cause0=0x2 epc0=0x800002cc cause1=0x2 epc1=0x800002d0 cause2=0x2 epc2=0x800002e0 cause3=0x9 epc3=0x800002e4 satp-now=0x0
phase2: mstatus-after-clear=0xa000000a0
phase2: new-traps=1 ecall-cause=0x9 ecall-epc=0x800002e4 satp-rb=0x0
Checks: 19
Mismatches: 0
Checksum: 0x580db705e095b710
Environment: QEMU 8.2.2
Verdict: PASS
```
