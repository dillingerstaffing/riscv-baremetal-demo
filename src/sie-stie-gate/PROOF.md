<!-- PROOF-HEADER
Checks: 18
Mismatches: 0
Checksum: 0xf9ecc89d6da3d570
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: sie.STIE as the S-mode per-interrupt enable gate

## What was built

`src/sie-stie-gate/`: a bare-metal RISC-V program that verifies the
gate behavior of the `sie.STIE` bit on the QEMU `virt` board. Three
files, about 500 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos. Exactly one mechanism is under
test: `sie.STIE` gates delivery of the supervisor timer interrupt
independently of the global `sstatus.SIE` gate. A pended STIP must
stay pending without trapping while STIE is clear (even with SIE
set), and must trap exactly once STIE is set.

- `stg_trap.S`: S-mode trap entry. Swaps t0 with sscratch, saves every
  general-purpose register, then calls the C handler
  `stg_trap_handler()` on a dedicated trap stack. The handler takes
  the one expected supervisor timer trap, records scause/sepc,
  disarms the timer by writing all-ones to `stimecmp` (the source is
  level-triggered, so leaving it armed would re-fire), and counts any
  further trap as unexpected. The M-mode vector `m_trap_entry` is a
  minimal record-and-park handler; no M-mode trap is expected after
  boot, and one would show up in the log as a FAIL rather than a
  silent hang. The register save area matches `stg_save_t` exactly:
  GPRs are stored at offsets 32 through 256, so the last store lands
  at the end of the 264-byte struct and no store reaches past it.
- `stg_main.c`: M-mode boot (probe Sstc via `menvcfg.STCE`, disarm
  both comparators, open one PMP NAPOT entry, grant the
  cycle/time counters via `mcounteren`, delegate the supervisor
  timer interrupt via `mideleg` bit 5, install direct-mode
  `stvec`, `mret` into S-mode) and the S-mode two-phase payload:
  phase A opens global `sstatus.SIE`, arms `stimecmp` with STIE
  clear, and watches `sip` STIP go pending across a
  100,000-rdcycle window with zero traps; phase B sets STIE via
  `csrs` (bit 5 is not encodable in the 5-bit `csrsi` immediate)
  exactly inside the labeled wait region (`stg_loop` /
  `stg_done`), takes the one trap, and runs a 100,000-rdcycle
  quiet window requiring no re-delivery. A failed check prints
  `FAIL` and flips the verdict; `RESULT: PASS` is printed only
  when every check held. On PASS the machine is shut down via the
  virt test-device finisher so the QEMU process exits 0; on FAIL
  the hart parks.
- `PROOF.md` (this file), plus an empty `bench-logs` placeholder
  matching the convention of the other modules.

Build: `make sie-stie-gate.elf` (added to `all` in the Makefile).
Run: `timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel sie-stie-gate.elf`
(or `make run-sie-stie-gate`).

## Configuration under test

- Hart: mhartid = 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the module then drops to S-mode itself.
- Sstc extension present (`menvcfg.STCE` sticks when set, probed at
  boot); `stimecmp` CSR 0x14d is used for the S-mode arm and disarm.
- `sstatus.SIE` is set for the whole S-mode run, so it cannot be the
  thing blocking delivery; only `sie` bit 5 (STIE) is under test. No
  software, no external interrupts, no PLIC involvement.
- `mideleg = 0x1464` (supervisor timer interrupt bit 5 plus the
  read-only default supervisor-interrupt delegation bits).

## Sequence and controls

1. Phase A, per-interrupt gate closed: `sstatus.SIE` set (read back
   as 1), `sie.STIE` explicitly clear (read back as 0).
   `stimecmp = mtime + 5000` (500 us of the 10 MHz virtual
   timebase); the readback is compared against the programmed value
   and only the match outcome is printed.
2. Gated window: 100000 `rdcycle` reads with STIE clear. mtime runs
   past stimecmp inside the window. The trap counter must stay 0
   while `sip` STIP is observed at 1: the interrupt is pending but
   the per-interrupt gate holds it back, even though the global SIE
   gate is open. The window is a bounded read count, so it always
   terminates. STIE is re-read at the end of the window and must
   still be clear.
3. Phase B, gate open: the single `csrs sie, t0` (t0 = 0x20)
   transition happens inside the labeled region between `stg_loop`
   and `stg_done`, then a bounded spin waits for the trap. Exactly
   one trap must arrive with `scause == 0x8000000000000005`
   (interrupt bit set, code 5 = supervisor timer interrupt) and
   `sepc` inside the labeled region. The handler's disarm is
   verified by readback: `stimecmp` reads all-ones and `sip` STIP
   reads 0. `sret` restores SIE from SPIE, which the trap entry
   saved as 1.
4. Quiet window: 100000 `rdcycle` reads with STIE on and `stimecmp`
   disarmed. The trap counter must stay 1: no re-delivery.

## A bug the verification caught

The first build of the trap entry stored the 29 general-purpose
registers at offsets 48 through 272, matching the sibling
`sstatus-sie-gate` module's entry code, while the C struct
`stg_save_t` places `gpr[29]` at offsets 32 through 256 (the struct
is 264 bytes: four header words plus 29 registers). The two
out-of-struct 8-byte stores (`t5` at 264, `t6` at 272) landed on
the BSS words following the save area. The `t6` store spans
`fails` and the full 4 bytes of the 32-bit `checks` counter; the
interrupted `t6` was 0, so `checks` was silently zeroed by the one
trap and the run printed `checks=7` instead of the true 18. The
fix moves the register stores to offsets 32 through 256, exactly
covering `gpr[29]` with the last store landing at the struct end.
After the fix the run prints `checks=18 fails=0`. The sibling
module shares the old entry layout and its `checks=7` line is the
same artifact; it is out of scope for this module.

## Results (three QEMU runs)

Three runs were performed after the final build; all print
`RESULT: PASS` with `checks=18 fails=0`, and all three are fully
byte-identical UART output. The table below is from the final
build; every value is a measured register read or counter, never a
computation from an assumption.

| step | run1 | run2 | run3 |
|---|---|---|---|
| `menvcfg.STCE` probe | present | present | present |
| `mideleg` readback | 0x1464 | 0x1464 | 0x1464 |
| `stimecmp` at S-mode entry | 0xffffffffffffffff | 0xffffffffffffffff | 0xffffffffffffffff |
| `sstatus.SIE` after enable (expect 1) | 1 | 1 | 1 |
| `sie.STIE` at phase A (expect 0) | 0 | 0 | 0 |
| stimecmp readback matches programmed | 1 | 1 | 1 |
| gated reads done (expect 100000) | 100000 | 100000 | 100000 |
| `sip` STIP observed in gated window (expect 1) | 1 | 1 | 1 |
| traps during gated window (expect 0) | 0 | 0 | 0 |
| `sie.STIE` after gated window (expect 0) | 0 | 0 | 0 |
| trap scause | 0x8000000000000005 | 0x8000000000000005 | 0x8000000000000005 |
| trap sepc (diagnostic, inside wait loop) | 0x80000388 | 0x80000388 | 0x80000388 |
| traps after gate opened (expect 1) | 1 | 1 | 1 |
| `sie.STIE` after gate open (expect 1) | 1 | 1 | 1 |
| `stimecmp` after handler (expect all-ones) | 0xffffffffffffffff | 0xffffffffffffffff | 0xffffffffffffffff |
| `sip` STIP after handler (expect 0) | 0 | 0 | 0 |
| traps after quiet window (expect 1) | 1 | 1 | 1 |
| checks / mismatches | 18 / 0 | 18 / 0 | 18 / 0 |
| FNV-1a checksum of verdict values | 0xf9ecc89d6da3d570 | 0xf9ecc89d6da3d570 | 0xf9ecc89d6da3d570 |
| RESULT | PASS | PASS | PASS |

What each value means:

- `sip` STIP = 1 with `traps = 0` in the gated window: the Sstc
  comparator went pending (mtime passed stimecmp) and the sip bit
  reflects it, but with `sie.STIE` clear the interrupt was not
  taken, even though the global `sstatus.SIE` gate stood open the
  whole time. Pending state, the global gate, and the
  per-interrupt gate are three independent facts; the CSR reads
  show it, not an assumption.
- `scause = 0x8000000000000005` on the single trap after STIE is
  set: the interrupt bit (63) set with exception code 5, i.e. a
  supervisor timer interrupt, exactly the delivery the armed
  `stimecmp` is specified to produce, arriving only once the
  per-interrupt gate opened.
- `sepc` inside the labeled wait region: the trap was taken at an
  instruction boundary of the gate-open wait loop, the only place
  in the run where STIE transitions from 0 to 1.
- `stimecmp = 0xffffffffffffffff` and `sip` STIP = 0 after the
  handler: the handler's disarm is confirmed by readback; STIP
  drops because the level-triggered source is gone, and the quiet
  window then shows no re-delivery while STIE stays on.
- `traps = 1` after the quiet window: the pended interrupt
  produced exactly one trap, no more, and no trap ever fired with
  the gate closed.
- The checksum is 64-bit FNV-1a over the deterministic measured
  verdict values (gated trap count, gated STIP observation, total
  trap count, scause, final stimecmp readback, final STIP bit,
  quiet-window trap delta, STIE readback after release). It is
  identical across runs because every measured value is identical.
- The armed `stimecmp` value and `sepc` are excluded from the
  checksum: the armed value depends on boot-time mtime (host
  timing), and `sepc` depends on which wait-loop instruction the
  interrupt lands on (a few instructions of range). Both are
  printed as diagnostics; the verdict lines are byte-identical
  across runs.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's Sstc, sip, sie, and sstatus CSR model
  on the `virt` machine, not real silicon. The STIE gate behavior
  is architectural, but the observation is against the emulator.
- Only hart 0, only the supervisor timer interrupt, only direct
  S-mode trap entry. External and software interrupts, vectored
  mode, and multi-hart behavior are not tested; the module is
  deliberately that small.
- The gated and quiet windows are 100000 `rdcycle` reads each,
  finite by construction. A leak past the gate or a late
  re-delivery would have shown up as a counter change inside those
  windows.

## Reproduction

```
make sie-stie-gate.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel sie-stie-gate.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.

## Build log (final build)

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sie-stie-gate/stg_trap.S -o src/sie-stie-gate/stg_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sie-stie-gate/stg_main.c -o src/sie-stie-gate/stg_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o sie-stie-gate.elf src/boot.o src/uart.o src/sie-stie-gate/stg_trap.o src/sie-stie-gate/stg_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: sie-stie-gate.elf has a LOAD segment with RWX permissions
```

No compiler warnings. The RWX linker warning is the norm for
these bare-metal modules (one RWX LOAD segment by design of the
linker script).

## Full console output (run 1 of 3)

```
sie-stie-gate: S-mode per-interrupt enable test
Sstc probe: menvcfg.STCE sticks (present)
mideleg=0x1464
dropping to S-mode
in S-mode: opening global sstatus.SIE, keeping sie.STIE clear
stimecmp at S-mode entry=0xffffffffffffffff
phase A: arming stimecmp = mtime+5000 ticks with STIE clear
arm: stimecmp-readback-match=1 (expect 1)
gated: reads-done=100000 sip.STIP-observed=1 traps-during-window=0 (expect 100000 / 1 / 0)
phase B: opening sie.STIE inside the labeled wait loop
trap1: scause=0x8000000000000005 sepc=0x80000388 traps=1
wait loop bounds: stg_loop=0x80000380 stg_done=0x80000398 (diagnostic: sepc varies by a few instructions per run)
disarm: stimecmp=0xffffffffffffffff sip.STIP=0 (expect 0xffffffffffffffff / 0)
quiet window: 100000 rdcycle reads with STIE on, stimecmp=all-ones
quiet: traps=1 (expect 1)
VERDICT gated_traps=0 gated_stip_observed=1 gate_open_traps=1 quiet_extra_traps=0 scause=0x8000000000000005 stimecmp_final=0xffffffffffffffff stip_final=0 checksum=0xf9ecc89d6da3d570
checks=18 fails=0
RESULT: PASS
```

Runs 2 and 3 are byte-identical to run 1 (verified with diff).
