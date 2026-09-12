<!-- PROOF-HEADER
Checks: 14
Mismatches: 0
Checksum: 0x48849bcd4202266a
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: the M-mode sret trap switch mstatus.TSR

## What was built

`src/mstatus-tsr-trap/`: a bare-metal RISC-V program that verifies
the TSR (Trap SRET) bit, bit 22 of mstatus, gates S-mode execution
of sret on the QEMU `virt` board. Two files, sharing only
`src/boot.S` and `src/uart.c` with the other demos, wired into the
Makefile as `mstatus-tsr-trap.elf` (boot.o first in link order).

- `tsr_trap.S`: the M-mode trap entry (saves all registers on the
  current stack, calls the C handler, restores, mret), the two
  M-mode phase drivers (`tsr_phase1`, `tsr_phase2`), and the shared
  S-mode probe pad. Each phase driver arms sepc with the pad, sets
  sstatus SPP=1 (leaving TSR exactly as M-mode established it:
  clear in phase 1, set in phase 2), records its resume label
  address with an in-asm label (never hardcoded) plus the ecall
  expectation flag, then sret. The pad re-arms SPP=1 (the driver's
  sret cleared it to U) and sepc to the post-sret resume point,
  executes the probe sret, then issues one ecall back to M-mode.
  The sret site and the ecall site are global labels, and both
  probe instructions are emitted under `.option norvc` (4 bytes
  each), so the handler's fixed mepc+4 skip past the faulting sret
  always resumes at the ecall.
- `tsr_main.c`: the M-mode driver. Installs the vector, opens a PMP
  NAPOT region over the whole address space for S-mode (with no PMP
  entry, lower-privilege fetches fault), keeps medeleg/mideleg at
  zero so every trap lands in M-mode, keeps mie and mstatus.MIE
  clear so no interrupt can pollute the trap counts, then runs the
  two phases and 14 checks. The M-mode handler records
  cause/epc/mtval per trap, advances mepc by 4 past the expected
  phase-2 sret trap, and redirects the phase-end ecall back to the
  recorded M-mode resume label with MPP restored to M-mode.

## The claim, stated honestly

The privileged architecture (section 3.1.6) defines TSR as the
M-mode switch for S-mode return control: when TSR=1, an sret
executed in S-mode raises an illegal-instruction exception.
sstatus carries no TSR bit, so S-mode cannot observe or change the
switch, only feel its effect.

This module is distinct from its siblings: `mstatus-tvm-trap` tests
the TVM bit trapping satp access and sfence.vma, `mstatus-tw-trap`
tests the TW (timeout-wait) bit trapping wfi. This is the sret
gate: the operation is legal in S-mode with the switch clear and
illegal with it set.

## Results (identical across all 3 runs)

| # | Check | Result |
|---|-------|--------|
| 1 | TSR clear at boot (mstatus=0xa00000000) | pass |
| 2 | phase-1 trap count == 1 (ecall only) | pass |
| 3 | phase-1 trap cause == 0x9 (S-mode ecall) | pass |
| 4 | phase-1 ecall mepc == ecall site (0x800002e8) | pass |
| 5 | phase-1 ecall mepc inside the pad range | pass |
| 6 | TSR set via csrs reads back set (0xa004000a2) | pass |
| 7 | setting TSR disturbed no other mstatus bit (vs pre-set 0xa000000a2) | pass |
| 8 | phase-2 trap count == 2 (sret trap + ecall) | pass |
| 9 | phase-2 first trap cause == 0x2 (illegal instruction) | pass |
| 10 | phase-2 first trap mepc == sret site (0x800002e4) | pass |
| 11 | phase-2 second trap cause == 0x9 (S-mode ecall) | pass |
| 12 | phase-2 second trap mepc == ecall site (0x800002e8) | pass |
| 13 | TSR clear via csrc reads back clear | pass |
| 14 | boot mstatus restored bit-for-bit | pass |

14 checks, 0 mismatches, FNV-1a checksum 0x48849bcd4202266a over
the deterministic verdict values, byte-identical across 3 QEMU
8.2.2 runs. QEMU exited 0 on all three runs (the virt test-device
finisher fired, which the driver only touches on the all-pass
path).

## Limits of verification

- The probe covers the QEMU 8.2.2 `virt` implementation of TSR. A
  physical hart follows the same privileged-spec rule, but this
  module documents what this hart does.
- Only the S-mode case is probed; M-mode sret is used as the
  phase-driver transition in both phases and is unaffected by TSR.
- Single hart, interrupts fully disarmed; the trap counts are only
  meaningful because nothing else can trap.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mstatus-tsr-trap/tsr_trap.S -o src/mstatus-tsr-trap/tsr_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mstatus-tsr-trap/tsr_main.c -o src/mstatus-tsr-trap/tsr_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o mstatus-tsr-trap.elf src/boot.o src/uart.o src/mstatus-tsr-trap/tsr_trap.o src/mstatus-tsr-trap/tsr_main.o
ld: warning: mstatus-tsr-trap.elf has a LOAD segment with RWX permissions
```

(Toolchain: Ubuntu riscv64-unknown-elf-gcc 13.2.0. The RWX warning
comes from the repo's shared link.ld and appears for every module.)

Probe-site encoding check (objdump of the built ELF), confirming
the sret and the ecall are 4 bytes each so the mepc+4 skip past the
faulting sret is exact:

```
800002e4:  10200073    sret     (tsr_sret_site)
800002e8:  00000073    ecall    (tsr_ecall_site)
```

## Full console output (run 1 of 3; runs 2 and 3 byte-identical)

```
mstatus-tsr-trap: M-mode sret trap switch mstatus.TSR
boot: mstatus=0xa00000000
pad: sret-site=0x800002e4 ecall-site=0x800002e8
phase1: traps=1 ecall-cause=0x9 ecall-epc=0x800002e8
phase2: mstatus-before-set=0xa000000a2 mstatus-after-set=0xa004000a2
phase2: new-traps=2 cause0=0x2 epc0=0x800002e4 cause1=0x9 epc1=0x800002e8
Checks: 14
Mismatches: 0
Checksum: 0x48849bcd4202266a
Environment: QEMU 8.2.2
Verdict: PASS
```
