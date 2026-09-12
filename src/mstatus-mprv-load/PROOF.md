<!-- PROOF-HEADER
Checks: 20
Mismatches: 0
Checksum: 0xcaa47691de34383c
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mstatus.MPRV makes M-mode loads honor mstatus.MPP

## What was built

`src/mstatus-mprv-load/`: a bare-metal RISC-V program that verifies
the MPRV bit (bit 17 of mstatus) changes the privilege level used
for explicit M-mode data accesses on the QEMU `virt` board. Two
files, sharing only `src/boot.S` and `src/uart.c` with the other
demos, wired into the Makefile as `mstatus-mprv-load.elf` (boot.o
first in link order).

- `mprv_trap.S`: the M-mode trap entry. Its first instructions stash
  t0 in mscratch (the faulting context may hold a live t0) and clear
  MPRV before any data memory access: with MPRV=1 and MPP=U the
  handler's own stack saves would be privilege-checked as U-mode
  accesses and fault, so the bit must be off before the first sd.
  Then it saves every general-purpose register on the current stack,
  calls the C handler, restores, and mret.
- `mprv_main.c`: the M-mode driver. Installs the vector, zeroes
  medeleg/mideleg so every trap lands in M-mode, keeps mie and
  mstatus.MIE clear so no interrupt can pollute the trap counts,
  installs one unlocked PMP TOR entry over a 4 KiB test page with
  R=W=X=0, then runs four phases and 20 checks. The handler records
  mcause/mepc/mtval per trap and advances mepc by 4 past the one
  expected synchronous probe load.

The PMP entry is the discriminator: an unlocked entry does not
apply to M-mode, so M-mode loads from the test page succeed, while
the R=W=X=0 region denies every access checked as a lower
privilege, so a load checked as U-mode faults. MPRV is what selects
which privilege the load is checked under.

## The claim, stated honestly

The privileged architecture (section 3.1.6) defines MPRV as the bit
that makes M-mode loads and stores use the privilege level in the
MPP field instead of M-mode. The four phases test each combination
of the switch: MPRV=0/MPP=U (M-mode semantics, bypasses the
unlocked entry), MPRV=1/MPP=U (U-mode semantics, hits the deny-all
region), MPRV=1/MPP=M (M-mode semantics again), then MPRV=0 with
the PMP entry restored and the boot register state put back.

A sibling module, `mstatus-mprv-readback`, proves the bit itself is
writable and reads back; this module proves what the bit does to a
load. The readback module's hazard note (a load with MPRV=1/MPP=U
faults when no PMP entry grants U-mode access) is the mechanism
this module turns into the probe.

## Results (identical across all 3 runs)

| # | Check | Result |
|---|-------|--------|
| 1 | MPRV clear at boot (mstatus=0xa00000000) | pass |
| 2 | no PMP entry programmed at boot (pmpcfg0=0x0) | pass |
| 3 | pmpaddr0 zero at boot | pass |
| 4 | pmpaddr1 zero at boot | pass |
| 5 | pmpcfg0 readback 0x08 after programming (TOR, R=W=X=0, unlocked) | pass |
| 6 | pmpaddr0 readback matches page>>2 | pass |
| 7 | pmpaddr1 readback matches (page+4096)>>2 | pass |
| 8 | phase-A control load (MPRV=0, MPP=U) trapped nothing | pass |
| 9 | phase-A load returned the canary (0xc0ffee1234567890) | pass |
| 10 | phase-B probe (MPRV=1, MPP=U) trapped exactly once | pass |
| 11 | phase-B trap cause 0x5 (load access fault) | pass |
| 12 | phase-B mepc 0x800005ee, exactly the probe load site | pass |
| 13 | phase-B mtval 0x80002000, the faulting page address | pass |
| 14 | phase-B destination register kept the poison value (the faulting load never completed) | pass |
| 15 | phase-C control load (MPRV=1, MPP=M) trapped nothing | pass |
| 16 | phase-C load returned the canary | pass |
| 17 | phase-D PMP entry unchanged after the trap (pmpcfg0 still 0x08) | pass |
| 18 | phase-D load with MPRV=0 succeeded again (canary) | pass |
| 19 | pmpcfg0 zeroed back to the boot state | pass |
| 20 | boot mstatus restored bit-for-bit | pass |

20 checks, 0 mismatches, FNV-1a checksum 0xcaa47691de34383c over
the deterministic verdict values, byte-identical across 3 QEMU
8.2.2 runs. QEMU exited 0 on all three runs (the virt test-device
finisher fired, which the driver only touches on the all-pass
path).

## Limits of verification

- The probe covers the QEMU 8.2.2 `virt` implementation of MPRV
  with an unlocked deny-all PMP region. A physical hart with a
  different PMP granularity or a locked entry covering the test
  page would behave differently; the module documents what this
  hart does with this entry.
- Only loads are probed, not stores; the spec applies MPRV to both,
  but the module claims only what it measured.
- Single hart, interrupts fully disarmed; the trap counts are only
  meaningful because nothing else can trap.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mstatus-mprv-load/mprv_trap.S -o src/mstatus-mprv-load/mprv_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mstatus-mprv-load/mprv_main.c -o src/mstatus-mprv-load/mprv_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o mstatus-mprv-load.elf src/boot.o src/uart.o src/mstatus-mprv-load/mprv_trap.o src/mstatus-mprv-load/mprv_main.o
```

(Toolchain: Ubuntu riscv64-unknown-elf-gcc 13.2.0. No warnings.)

Probe-site encoding check (objdump of the built ELF), confirming
each probe load is 4 bytes and the csrc that clears MPRV sits
immediately after, so the mepc+4 skip is exact:

```
800005ee:  00093983    ld    s3,0(s2)      (mprv_load_site_u)
800005f2:  300cb073    csrc  mstatus,s9
...
800006fc:  00093403    ld    s0,0(s2)      (mprv_load_site_m)
80000700:  300cb073    csrc  mstatus,s9
```

## Full console output (run 1 of 3; runs 2 and 3 byte-identical)

```
mstatus-mprv-load: MPRV makes M-mode loads honor MPP
boot: mstatus=0xa00000000 pmpcfg0=0x0
test page=0x80002000
pmp: pmpaddr0=0x20000800 pmpaddr1=0x20000c00 pmpcfg0=0x8
sites: load-u=0x800005ee load-m=0x800006fc
phaseA: value=0xc0ffee1234567890 traps=0
phaseB: value=0xbadc0de0badc0de0 new-traps=1 cause=0x5 mepc=0x800005ee mtval=0x80002000
phaseC: value=0xc0ffee1234567890 new-traps=0
phaseD: value=0xc0ffee1234567890
Checks: 20
Mismatches: 0
Checksum: 0xcaa47691de34383c
Environment: QEMU 8.2.2
Verdict: PASS
```
