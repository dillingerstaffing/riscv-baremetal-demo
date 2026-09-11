<!-- PROOF-HEADER
Checks: 26
Mismatches: 0
Checksum: 0x56131e82d159a905
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: the S-mode interrupt gate bit driven from M-mode via mstatus.SIE

## What was built

`src/mstatus-sie-toggle/`: a bare-metal RISC-V program that verifies
the S-mode interrupt gate bit (bit 1 of mstatus) is under M-mode's
control on the QEMU `virt` board. Three files, sharing only
`src/boot.S` and `src/uart.c` with the other demos, wired into the
Makefile as `mstatus-sie-toggle.elf` (boot.o first in link order).

- `msg_trap.S`: M-mode and S-mode trap entries, the two M-mode phase
  drivers (`msg_phase1`, `msg_phase2`), and the S-mode landing pad.
  The M-mode entry saves all registers on the stack, calls the C
  handler, restores, and mret. The S-mode entry swaps t0 with
  sscratch, saves all registers, calls the C handler on a dedicated
  trap stack, restores, and sret. Each phase driver arms sepc with
  the landing pad address, sets sstatus SPP (leaving the gate bit
  exactly as M-mode established it), records its resume label address
  (computed with an in-asm label, never hardcoded) and the ecall
  expectation flag, then sret. The landing pad spins on rdcycle for a
  bounded 200000-cycle window, then issues one ecall back to M-mode.
- `msg_main.c`: the M-mode driver. Installs both vectors, opens a
  PMP NAPOT region over the whole address space for S-mode (with no
  PMP entry, lower-privilege fetches fault), delegates the supervisor
  software interrupt (mideleg bit 1, read back), enables it (sie.SSIE,
  read back), then runs the two phases and 26 checks.

## The claim, stated honestly

The privileged architecture defines sstatus as a subset view of
mstatus, so `mstatus.SIE` and `sstatus.SIE` are one physical bit, not
two gates. This module does not test a second gate; it tests the same
gate bit driven from M-mode through the mstatus CSR, which the
sibling `sstatus-sie-gate` module drives from S-mode. The aliasing is
verified inside the run itself: every mstatus.SIE write is read back
through both mstatus and sstatus (checks: the sstatus readback
follows the mstatus clear, and follows the mstatus set). The other
sibling, `sie-stie-gate`, tests a genuinely different bit (the
sie.STIE per-interrupt enable).

Phase 1 (M-mode clears mstatus.SIE, read back clear via mstatus and
sstatus): mip.SSIP is pended (read back set) and S-mode spins the
full bounded window. Required: zero S-mode traps, exactly one M-mode
trap (the return ecall, mcause 0x9, mepc at the ecall instruction),
mip.SSIP still pending (delivery was gated, not the pending state),
sie.SSIE still set, gate bit still clear.

Phase 2 (M-mode sets mstatus.SIE, read back set via mstatus and
sstatus; mip.SSIP still pending): the same pending interrupt traps
exactly once in S-mode with scause 0x8000000000000001 and sepc
exactly at the landing pad address. The handler records the cycle
counter, clears SSIP, and sret resumes the spin; the rest of the
window is the quiet window. Required: S-mode traps == 1, M-mode
traps == 2, mip.SSIP clear, gate bit still set, trap count unmoved,
and a long post-clear quiet window.

Two behaviors worth noting, both verified by the run rather than
assumed:

- The M-mode ecall handler must raise mstatus.MPP to M-mode before
  mret. The ecall arrives from S-mode, so the trap sets MPP=S, and a
  plain mret would return into S-mode (observed during development as
  an illegal-instruction trap on the next M-mode CSR access in main).
  The handler sets MPP=M, redirects mepc at the registered resume
  label, and mret resumes the M-mode driver.
- `sepc` of the phase-2 trap equals the landing pad address exactly
  (0x80000418): the pending interrupt is taken at the first S-mode
  instruction boundary after sret.

## Results (run 1 of 3)

| # | check | measured |
|---|-------|----------|
| 1 | boot medeleg leaves S-mode ecall to M-mode | medeleg=0x0, bit 9 clear |
| 2 | mie clears | mie=0x0 |
| 3 | mideleg bit 1 sticks | readback 0x1446 |
| 4 | sie.SSIE sticks | readback 0x2 |
| 5 | mstatus.SIE clears | readback bit 1 = 0 |
| 6 | sstatus.SIE follows the mstatus clear (aliasing) | readback bit 1 = 0 |
| 7 | mip.SSIP pends | readback 0x82, bit 1 = 1 |
| 8 | no S-mode trap with gate closed | s_traps=0 |
| 9 | one M-mode trap after phase 1 | m_traps=1 |
| 10 | phase-1 trap is the S-mode ecall | mcause=0x9 |
| 11 | phase-1 ecall mepc in pad range | 0x80000432 in [0x80000418,0x80000432] |
| 12 | mip.SSIP stays pending through phase 1 | bit 1 = 1 |
| 13 | sie.SSIE stays set through phase 1 | bit 1 = 1 |
| 14 | gate bit stays clear through phase 1 | bit 1 = 0 |
| 15 | mstatus.SIE sets | readback bit 1 = 1 |
| 16 | sstatus.SIE follows the mstatus set (aliasing) | readback bit 1 = 1 |
| 17 | mip.SSIP stays pending into phase 2 | bit 1 = 1 |
| 18 | exactly one S-mode trap with gate open | s_traps=1 |
| 19 | trap is the supervisor software interrupt | scause=0x8000000000000001 |
| 20 | sepc at the landing pad | 0x80000418 in range |
| 21 | two M-mode traps after phase 2 | m_traps=2 |
| 22 | phase-2 trap is the S-mode ecall | mcause=0x9 |
| 23 | handler cleared the pending bit | mip bit 1 = 0 |
| 24 | gate bit stays set through phase 2 | bit 1 = 1 |
| 25 | trap count unmoved in quiet window | s_traps=1 |
| 26 | quiet window long | 376125 cycles > 100000 |

Diagnostics (not checks): boot mideleg=0x1444 (QEMU ORs hypervisor
bits in; bit 1 added by the setup write), mip=0x82 at pend time (bit 7
is a pre-existing pending machine timer interrupt, never enabled in
mie, never fires), mstatus boot value 0xa00000000 (the UXL/SXL XLEN
fields).

Checksum inputs (FNV-1a 64, printed as 0x56131e82d159a905):
mideleg_rb=0x1446, sie_rb=0x2, mstatus_p1.SIE=0, sstatus_p1.SIE=0,
mip_pended.SSIP=1, mstatus_p2.SIE=1, sstatus_p2.SIE=1, s_traps=1,
m_traps=2, s_cause=0x8000000000000001, sepc_in_range=1,
mip_final.SSIP=0, checks=26.

## Limits of verification

This was verified on the QEMU 8.2.2 `virt` emulator (TCG), not on
silicon. The delegation, gating, and CSR-aliasing behaviors observed
are those of QEMU's implementation (checked against
target/riscv/cpu_helper.c at v8.2.2 during development); a hardware
hart implements the same privileged-spec rules, but this run cannot
prove silicon behavior. The quiet-window cycle count is a
host-timing-dependent diagnostic: it varies run to run (376125 /
2842305 / 778575 across the three runs) because rdcycle on this
QEMU is wall-clock backed and the count includes the M-mode
return path and UART printing after the ecall. It is excluded from
the checksum; every verdict-relevant value above was byte-identical
across all three runs. Single hart only; no claim is made about
multi-hart interrupt behavior.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mstatus-sie-toggle/msg_trap.S -o src/mstatus-sie-toggle/msg_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mstatus-sie-toggle/msg_main.c -o src/mstatus-sie-toggle/msg_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o mstatus-sie-toggle.elf src/boot.o src/uart.o src/mstatus-sie-toggle/msg_trap.o src/mstatus-sie-toggle/msg_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: mstatus-sie-toggle.elf has a LOAD segment with RWX permissions
```

(The RWX LOAD segment warning is standard for this repo's link.ld
and appears for every module build.)

QEMU used: /home/hatch/workspace/qemu/usr/bin/qemu-system-riscv64
(8.2.2) with LD_LIBRARY_PATH=/home/hatch/workspace/qemu/usr/lib/x86_64-linux-gnu:/home/hatch/workspace/qemu/lib/x86_64-linux-gnu,
`-machine virt -nographic -bios none -kernel mstatus-sie-toggle.elf`
under `timeout 20`. All three runs exited 0 (the module powers the
machine off via the virt test-device finisher on PASS).

## Full console output (run 1 of 3)

```
mstatus-sie-toggle: S-mode interrupt gate driven from M-mode via mstatus.SIE
boot: mideleg=0x1444 medeleg=0x0 mie=0x0 mstatus=0xa00000000 sie=0x0
setup: mideleg-after-delegate=0x1446
setup: sie-after-enable=0x2
phase1: mstatus-after-clear=0xa00000000 sstatus-after-clear=0x200000000
phase1: mip-after-pend=0x82
phase1: pad=[0x80000418,0x80000432]
phase1: s_traps=0 m_traps=1 mcause=0x9 mepc=0x80000432
phase2: mstatus-after-set=0xa000000a2 sstatus-after-set=0x200000022 mip=0x82
phase2: s_traps=1 scause=0x8000000000000001 sepc=0x80000418 m_traps=2 quiet-cycles=376125
Checks: 26
Mismatches: 0
Checksum: 0x56131e82d159a905
Environment: QEMU 8.2.2
Verdict: PASS
```

Runs 2 and 3 produced byte-identical output except the diagnostic
`quiet-cycles` value (2842305 and 778575; host-timing-dependent, not
part of the verdict or the checksum).
