<!-- PROOF-HEADER
Checks: 15
Mismatches: 0
Checksum: 0xfe7b4a55488e1c01
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: sie WARL write/legalized-readback probe of STIE

## What was built

`src/sie-stie-write/`: a bare-metal RISC-V program that probes the
`sie` CSR (supervisor interrupt enable, CSR 0x104) as a WARL register
in M-mode. Four files, about 400 lines total, sharing only
`src/boot.S` and `src/uart.c` with the other demos. Exactly one
mechanism is under test: the write/readback legalization of the `sie`
enable bits, and specifically whether `sie.STIE` (bit 5) is admitted
by a write and round-trips through set/clear. No interrupt source is
armed or pended and no trap is expected; the trap count stays 0 for
the whole run.

- `stw_trap.S`: M-mode trap entry. Counts the trap into
  `stw_trap_count` and parks the hart without returning. No trap is
  expected (MIE stays clear, no interrupt source is armed), so any
  stray trap shows up as a harness timeout, and reaching the verdict
  line proves the counter stayed 0.
- `stw_main.c`: M-mode boot (`uart_init`, defensive `mtvec`,
  `mideleg` delegation of the supervisor timer interrupt only),
  then the probe sequence: boot `sie`/`mideleg` baselines, an
  all-ones write to `sie` with the legalized readback published,
  a zero write, `csrs` set of STIE, `csrc` clear of STIE, and a
  restore of `sie` to 0 and `mideleg` to its boot value, every step
  readback-verified. A failed check prints `FAIL` and flips the
  verdict; `RESULT: PASS` is printed only when every check held.
  On PASS the machine is shut down via the virt test-device
  finisher so the QEMU process exits 0; on FAIL the hart parks.
- `README.md` (this module's index entry), plus an empty
  `bench-logs` placeholder matching the convention of the other
  modules.

Build: `make sie-stie-write.elf` (added to `all` in the Makefile).
Run: `timeout 20 qemu-system-riscv64 -machine virt -nographic -bios none -kernel sie-stie-write.elf`
(or `make run-sie-stie-write`).

## Distinction from the sibling module

`src/sie-stie-gate/` tests interrupt-gating behavior: a pended STIP
with STIE clear produces no trap in S-mode, and setting STIE
produces exactly one trap. That module arms the supervisor timer,
pends an interrupt, drops to S-mode, and takes traps. This module
does none of that: it stays in M-mode, never arms or pends any
interrupt source, and takes no traps. The mechanism here is
register legalization (which bits a WARL write to `sie` admits),
not interrupt delivery. The two modules test different mechanisms
and share no behavior.

## Configuration under test

- Hart: mhartid = 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the module never leaves M-mode.
- `mstatus.MIE` stays clear for the whole run and no `mie` enable
  bit is ever set, so no interrupt can be taken regardless of
  pending state.
- QEMU 8.2.2 `virt` machine, `LD_LIBRARY_PATH` pointed at the
  pinned toolchain's QEMU libraries.

## Expected values: measured first, then asserted

The asserted constants were measured with a scratch probe
(`/tmp/ssw-probe.c`, never committed) run once under the same
QEMU before the module was written. Verbatim probe transcript:

```
probe: sie WARL measurement
boot sie=0x0 boot mideleg=0x1444
sie all-ones at boot mideleg -> 0x0
mideleg write 0x20 -> 0x1464
sie all-ones at mideleg=0x1464 -> 0x20
sie zero write -> 0x0
csrs STIE -> 0x20
csrc STIE -> 0x0
mideleg all-ones -> 0x3666
sie all-ones at full mideleg -> 0x2222
restored sie=0x0 mideleg=0x1444
DONE
```

Reading: at boot `mideleg` is 0x1444 (the forced H-extension bits)
and no S-mode interrupt is delegated, so an all-ones write to
`sie` legalizes to 0x0 (nothing is admitted). Delegating only the
supervisor timer interrupt (`mideleg` write 0x20, readback 0x1464)
makes the all-ones write legalize to 0x20: STIE alone sticks,
because SSIE and SEIE have no delegated interrupt behind them.
`csrs`/`csrc` of STIE round-trip the bit (0x20 / 0x0). The
exploratory full-delegation readback (0x2222 = bits 1, 5, 9, 13) is
consistent with "a bit sticks exactly when its interrupt is
delegated" but was not asserted by the module, which delegates
only STI.

## Sequence and controls

1. Boot baselines: `sie` reads 0x0, `mideleg` reads 0x1444. The
   defensive `mtvec` is installed and read back (base matches the
   handler address, mode bits 0).
2. `mideleg` = 0x20 written; readback must be 0x1464 (forced bits
   plus the delegated STI).
3. `sie` = all-ones written; readback must be 0x20. STIE set,
   SSIE and SEIE clear: the admitted set follows the delegation.
4. `sie` = 0 written; readback must be 0x0.
5. `csrs sie, t0` with t0 = 0x20 (bit 5 is not encodable in the
   5-bit `csrsi` immediate); readback must be 0x20, the bit set
   and alone.
6. `csrc sie, t0`; readback must be 0x0.
7. Restore: `sie` written 0 (readback 0x0), `mideleg` written
   0x1444 (readback 0x1444).
8. Trap count read from `stw_trap_count` must be 0.

All waits are bounded by the UART drain poll and the harness
timeout, never open ended. Every printed value in the verdict
section is a register readback or the trap counter, so the
verdict lines are byte-identical across runs by construction;
no host-timing-dependent value is printed anywhere.

## Build log (genuine)

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sie-stie-write/stw_trap.S -o src/sie-stie-write/stw_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sie-stie-write/stw_main.c -o src/sie-stie-write/stw_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o sie-stie-write.elf src/boot.o src/uart.o src/sie-stie-write/stw_trap.o src/sie-stie-write/stw_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: sie-stie-write.elf has a LOAD segment with RWX permissions
```

(The RWX warning is the standard bare-metal link-script warning,
present for every module in this repo; `boot.o` is first in the
link order, so `_start` runs at the 0x80000000 load address.)

## Results: 3 runs, byte-identical

Each run: `timeout 20 qemu-system-riscv64 -machine virt
-nographic -bios none -kernel sie-stie-write.elf`, exit status 0
(finisher shutdown). `diff` of the three logs: empty (byte-identical).

Run 1 (runs 2 and 3 identical):

```
sie-stie-write: sie WARL write/legalized-readback probe of STIE
boot: sie=0x0 mideleg=0x1444
mideleg: write=0x20 readback=0x1464 (expect 0x1464)
sie: write=0xffffffffffffffff readback=0x20 (expect 0x20)
sie: write=0x0 readback=0x0 (expect 0x0)
sie: csrs STIE readback=0x20 (expect 0x20)
sie: csrc STIE readback=0x0 (expect 0x0)
restore: sie=0x0 mideleg=0x1444 (expect 0x0 / 0x1444)
VERDICT sie_boot=0x0 mideleg_boot=0x1444 mideleg_sti=0x1464 sie_ones=0x20 sie_zero=0x0 sie_csrs=0x20 sie_csrc=0x0 sie_final=0x0 mideleg_final=0x1444 traps=0 checksum=0xfe7b4a55488e1c01
checks=15 fails=0
RESULT: PASS
```

| Run | Exit | Checksum           | Verdict |
|-----|------|--------------------|---------|
| 1   | 0    | 0xfe7b4a55488e1c01 | PASS    |
| 2   | 0    | 0xfe7b4a55488e1c01 | PASS    |
| 3   | 0    | 0xfe7b4a55488e1c01 | PASS    |

The checksum is FNV-1a-64 over the ten deterministic measured
values (nine register readbacks plus the trap counter).

## What was verified, exactly

- `sie` reads 0x0 at M-mode boot on this hart.
- `mideleg` reads 0x1444 at boot and accepts the STI delegation
  write (readback 0x1464, forced bits intact).
- An all-ones write to `sie` with only STI delegated legalizes to
  0x20: STIE sticks, SSIE and SEIE do not (their interrupts are not
  delegated).
- A zero write to `sie` reads back 0x0.
- `csrs` sets STIE and the readback is 0x20; `csrc` clears it and
  the readback is 0x0.
- `sie` restores to 0x0 and `mideleg` restores to 0x1444,
  readback-verified.
- Zero traps fired across the whole run (counter read 0 with a
  counting park-on-entry vector installed).

Limits: the legalization rule is asserted only for the STI-only
delegation the module programs. The full-delegation readback
(0x2222) was measured in the exploratory probe but is not part of
the module's asserted surface. Nothing in this module exercises
interrupt delivery; for that, see `src/sie-stie-gate/`.
