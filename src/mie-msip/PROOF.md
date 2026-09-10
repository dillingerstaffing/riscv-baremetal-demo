# Proof: mie.MSIE bit gates machine software interrupt delivery (backlog item 112)

## What was built

`src/mie-msip/`: a bare-metal RISC-V program that verifies the MSIE
bit (bit 3) of the mie CSR gates delivery of the machine software
interrupt raised by the CLINT msip register on the QEMU `virt`
board. The global `mstatus.MIE` bit stays set for the whole run, so
`mie.MSIE` is the only gating variable. Three files, sharing only
`src/boot.S` and `src/uart.c` with the other demos. Exactly one
mechanism is under test: writing mie to clear MSIE stops delivery
with msip asserted, writing mie to set MSIE lets the pending
interrupt trap exactly once.

- `mie_main.c`: verifies boot state (mie == 0, mstatus.MIE == 0),
  installs a direct-mode mtvec handler with an mscratch scratch
  area, sets mstatus.MIE (read back), then runs phase 1 (mie = 0
  via `csrw`, read back, msip set, bounded quiet window, zero traps
  allowed), phase 2 (mie = 0x8 via `csrsi`, read back, msip
  clear-then-set with MIE paused around set+readback, exactly one
  trap with mcause `0x8000000000000003`, no re-delivery), and a
  re-gate check (mie = 0 via `csrci`, read back, fresh msip set
  traps nothing). A failed check prints `FAIL` and flips the
  verdict; `RESULT: PASS` is printed only when every check held. On
  PASS the machine shuts down through the virt test-device finisher
  (QEMU exits 0); on FAIL the hart parks without touching the
  finisher.
- `mie_trap.S`: trap entry that saves t0/t1/ra/a0 through
  mscratch, records mcause/mepc/mtval and an entry cycle stamp,
  calls the C handler (clears msip first, since the source is
  level-triggered, then records the values and bumps the trap
  counter), restores, and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build (xPack riscv-none-elf-gcc 15.2.0; see toolchain note):
`riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o mie-msip.elf src/boot.o src/uart.c src/mie-msip/mie_main.c src/mie-msip/mie_trap.S`

Run: `timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mie-msip.elf`
with `~/workspace/qemu/usr/bin/qemu-system-riscv64` (8.2.2) and
`LD_LIBRARY_PATH=~/workspace/qemu/usr/lib/x86_64-linux-gnu:~/workspace/qemu/lib/x86_64-linux-gnu`
(the compat-bin QEMU is broken; missing libfdt/libfuse3).

## Configuration under test

- Hart: hart 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- CLINT msip for hart 0 at `0x02000000`, accessed as a 32-bit
  register (this QEMU's CLINT model only accepts 4-byte accesses to
  msip; see the defect note in `src/msip/PROOF.md`, backlog item
  70).
- The machine software interrupt is the only enabled interrupt
  source: mie.MTIE and mie.MEIE are never set, so the mcause value
  and the trap counts are the ground truth for the gate.
- `mstatus.MIE = 1` from setup to the end (read back at both
  ends); mie.MSIE is the only bit that changes mid-run.

## Sequence and controls

1. Control: at boot, mie must read `0x0` and mstatus.MIE must read
   0. The trap vector is installed and verified (direct mode), then
   mstatus.MIE is set and read back.
2. Phase 1 (MSIE clear): `csrw mie, zero`; the full mie readback
   must be `0x0`. msip is set to 1 (readback 1) and a bounded
   quiet window runs; the trap counter must stay 0, proving the
   pending bit sits in mip without being delivered. msip is
   cleared (readback 0).
3. Phase 2 (MSIE set): `csrsi mie, 8`; the full mie readback must
   be `0x8` (only bit 3 moved). msip is cleared then re-set so the
   phase-2 trigger is deliberate; mstatus.MIE is paused around the
   set+readback (same construction as `src/msip/`) so the handler
   cannot clear msip between the store and the load, then MIE is
   restored and the trap must fire. The handler must report
   mcause `0x8000000000000003` and the counter must be exactly 1;
   a further quiet window must leave it at 1 (no re-delivery,
   handler cleared msip).
4. Re-gate: `csrci mie, 8`; the full mie readback must be `0x0`.
   A fresh msip set (readback 1) and quiet window must leave the
   counter at 1, proving the mie write alone stops delivery. msip
   is cleared and mstatus.MIE is re-read to confirm it never
   moved.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

The three run logs are byte-identical except for the run
captured; the table shows run1, and runs 2 and 3 matched every
value.

| step | run1 | run2 | run3 |
|---|---|---|---|
| mie at boot (expect 0x0) | 0x0 | 0x0 | 0x0 |
| mstatus.MIE at boot (expect 0) | 0 | 0 | 0 |
| mtvec (handler installed, direct) | 0x80000830 | 0x80000830 | 0x80000830 |
| mstatus.MIE after setup (expect 1) | 1 | 1 | 1 |
| phase1: mie after `csrw mie, zero` (expect 0x0) | 0x0 | 0x0 | 0x0 |
| phase1: msip readback after set (expect 1) | 1 | 1 | 1 |
| phase1: traps with MSIE clear (expect 0) | 0 | 0 | 0 |
| phase1: msip readback after clear (expect 0) | 0 | 0 | 0 |
| phase2: mie after `csrsi mie, 8` (expect 0x8) | 0x8 | 0x8 | 0x8 |
| phase2: msip readback on re-set (expect 1) | 1 | 1 | 1 |
| phase2: poll spins to trap (expect < 10000000) | 0 | 0 | 0 |
| phase2: mcause (expect 0x8000000000000003) | 0x8000000000000003 | 0x8000000000000003 | 0x8000000000000003 |
| phase2: mepc | 0x80000450 | 0x80000450 | 0x80000450 |
| phase2: trap count (expect 1) | 1 | 1 | 1 |
| phase2: msip after handler (expect 0) | 0 | 0 | 0 |
| phase2: traps after quiet (expect 1) | 1 | 1 | 1 |
| re-gate: mie after `csrci mie, 8` (expect 0x0) | 0x0 | 0x0 | 0x0 |
| re-gate: msip readback after set (expect 1) | 1 | 1 | 1 |
| re-gate: traps with MSIE clear again (expect 1) | 1 | 1 | 1 |
| end: mstatus.MIE (expect 1) | 1 | 1 | 1 |
| QEMU exit code | 0 | 0 | 0 |
| RESULT | PASS | PASS | PASS |

What each value means:

- `mie` `0x0 -> 0x8 -> 0x0` with full-word readbacks: the CSR
  takes the writes exactly, and only bit 3 moves. The write
  forms exercised are `csrw` (full word), `csrsi` (set bit), and
  `csrci` (clear bit).
- Phase-1 trap count 0 with msip asserted, mstatus.MIE = 1, and
  mie.MSIE = 0: the pending interrupt is gated off by the mie
  bit alone, since the global bit was on and a handler was
  installed and verified.
- Phase-2 trap count 1 with mcause `0x8000000000000003`: the
  exact machine software interrupt fired when MSIE was set. The
  mepc `0x80000450` is the instruction after the `csrs
  mstatus` that re-enabled MIE, consistent with the trap being
  taken as soon as the global bit went back on with msip
  pending.
- `spins-to-trap = 0`: the trap had already been taken before
  the poll loop's first iteration (delivery happens on the MIE
  re-enable instruction), so the loop exits immediately; the
  check is `spins < 10000000`, and 0 satisfies it. This is
  prompt delivery, not a skipped poll.
- Traps after each quiet window unchanged (1 stays 1): the
  handler's msip clear ends the level-triggered source, and no
  spurious re-delivery occurs.
- Re-gate trap count still 1 after a fresh msip assertion with
  MSIE clear: clearing the mie bit stops delivery again,
  symmetric with phase 1.
- QEMU exit code 0 on all runs: the finisher shutdown path
  executed, i.e. `RESULT: PASS` with no parked FAIL.

## Toolchain note (measured, not assumed)

The distro `riscv64-unknown-elf-gcc` 13.2.0 was reported missing
mid-run by the item-111 worker, so this module was built and run
with the xPack GNU RISC-V Embedded GCC 15.2.0 at
`~/workspace/toolchains/xpack-riscv-none-elf-gcc-15.2.0-1`
(`riscv-none-elf-gcc` with `CROSS=riscv-none-elf-` semantics),
using the repo's Makefile flags (`-Wall -Wextra -O2
-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`). The build log
in `bench-logs/build.log` records the exact command; the only
diagnostic is the linker's benign RWX-LOAD-segment warning also
seen on the sibling builds. No labels-as-values are used anywhere
in the module, so the documented 13.2.0 `&&label` miscompile
does not apply.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's CSR and CLINT models on the `virt`
  machine, not real silicon. The mie.MSIE gating semantics are
  architectural; the 4-byte-only msip access width is this
  model's implementation detail (verified in `src/msip/`).
- Only hart 0, only M-mode, only the machine software interrupt
  enable bit. Timer enable (MTIE), external enable (MEIE),
  S-mode delegation, and multi-hart delivery are not tested
  here; the module is deliberately that small.
- The three runs are byte-identical; no host-varying values
  appear in the check path.

## Reproduction

```
export PATH="$HOME/workspace/toolchains/xpack-riscv-none-elf-gcc-15.2.0-1/bin:$PATH"
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o mie-msip.elf src/boot.o src/uart.c src/mie-msip/mie_main.c src/mie-msip/mie_trap.S
export LD_LIBRARY_PATH="$HOME/workspace/qemu/usr/lib/x86_64-linux-gnu:$HOME/workspace/qemu/lib/x86_64-linux-gnu"
timeout 30 ~/workspace/qemu/usr/bin/qemu-system-riscv64 -machine virt -nographic -bios none -kernel mie-msip.elf
```

Linked flat at 0x80000000 via `link.ld`. Build log:
`bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`.
Each ends with `RESULT: PASS (traps=1)` and the finisher shutdown
(exit 0).
