# Proof: CLINT msip software-interrupt delivery (backlog item 70)

## What was built

`src/msip/`: a bare-metal RISC-V program that verifies the CLINT
software-interrupt (msip) delivery path on the QEMU `virt` board. Two
files, about 330 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos. Exactly one mechanism is under
test: writing 1 to the CLINT msip register for hart 0 delivers a
machine software interrupt, and clearing msip ends delivery with no
re-fire.

- `msip_trap.S`: M-mode trap entry. Saves t0 via mscratch, records
  mcause/mepc/mtval plus an `rdcycle` stamp at handler entry, then
  calls the C handler `msip_c_handle()`, which clears msip first (the
  source is level-triggered: leaving it set would re-fire the instant
  `mret` restores MIE), bumps the trap counter, and records what the
  handler observed. All interrupted state is restored before `mret`.
- `msip_main.c`: installs direct-mode `mtvec`, enables `mie.MSIE` and
  `mstatus.MIE` (both read back), then runs two set/clear cycles with
  checks on every step. A failed check prints `FAIL` and flips the
  verdict; `RESULT: PASS` is printed only when every check held.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make msip.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel msip.elf`
(or `make run-msip`).

## Configuration under test

- Hart: mhartid = 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- CLINT msip for hart 0 at `0x02000000`, accessed as a 32-bit register
  (the only width this QEMU's CLINT model accepts for msip; see defect
  1 below).
- Only the machine software interrupt is enabled (`mie` bit 3); no
  timer, no external interrupts, no PLIC involvement.

## Sequence and controls

1. Control: with `mie.MSIE` and `mstatus.MIE` set but msip reading 0,
   a 1M-spin quiet window must produce zero traps. This proves the
   interrupt the test waits for comes from the msip write and not from
   spurious delivery.
2. Cycle 1: write 1 to msip, read it back (must be 1), wait for the
   trap with a 10M-spin budget. The handler must observe
   `mcause == 0x8000000000000003` (interrupt bit set, code 3 =
   machine software interrupt) and must have cleared msip (reads 0
   afterwards). A 2M-spin quiet window must leave the trap counter at
   1: no re-delivery.
3. Cycle 2: the same set/clear again, counter ending at 2, mcause
   checked again. This proves the path re-arms.
4. The set+readback is done with `mstatus.MIE` briefly cleared around
   it, so the handler cannot clear msip between the store and the
   load; the delivery-latency stamp (`rdcycle`) is taken just before
   MIE is re-enabled, so the handler entry stamp is always later and
   the latency is a true set-to-entry measurement.

## Clock facts used (all measured, none assumed)

`rdcycle` on this QEMU follows the host clock, not the 10 MHz `mtime`
timebase. The program calibrates it on every boot against the CLINT
`mtime` (same construction as `src/plic/`): 1,000,000 mtime ticks
(100 ms of virtual time) against the `rdcycle` delta. All three runs
measured ratio 149 `rdcycle` units per tick, i.e. about 1.49e9
units/second, host-clock driven. Delivery latencies below are
therefore host-time costs of emulated trap delivery (vCPU exit,
device/interrupt emulation, re-entry), not guest instruction counts.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

| step | run1 | run2 | run3 |
|---|---|---|---|
| `rdcycle`/`mtime` ratio | 149 | 149 | 149 |
| mtvec (direct mode) | 0x800001c8 | 0x800001c8 | 0x800001c8 |
| `mie.MSIE` / `mstatus.MIE` | 1 / 1 | 1 / 1 | 1 / 1 |
| msip before first set (expect 0) | 0 | 0 | 0 |
| traps with msip clear (expect 0) | 0 | 0 | 0 |
| msip readback after set 1 (expect 1) | 1 | 1 | 1 |
| trap1 spins to delivery | 0 | 0 | 0 |
| trap1 mcause | 0x8000000000000003 | 0x8000000000000003 | 0x8000000000000003 |
| trap1 mepc | 0x8000044e | 0x8000044e | 0x8000044e |
| trap1 delivery latency (rdcycle units) | 77670 | 79860 | 82155 |
| msip after handler clear 1 (expect 0) | 0 | 0 | 0 |
| traps after quiet window (expect 1) | 1 | 1 | 1 |
| msip readback after set 2 (expect 1) | 1 | 1 | 1 |
| trap2 spins to delivery | 0 | 0 | 0 |
| trap2 mcause | 0x8000000000000003 | 0x8000000000000003 | 0x8000000000000003 |
| trap2 mepc | 0x800005a0 | 0x800005a0 | 0x800005a0 |
| trap2 delivery latency (rdcycle units) | 27240 | 27255 | 34305 |
| msip after handler clear 2 (expect 0) | 0 | 0 | 0 |
| traps after quiet window (expect 2) | 2 | 2 | 2 |
| RESULT | PASS | PASS | PASS |

What each value means:

- `mcause = 0x8000000000000003` on both traps: the interrupt bit (63)
  set with exception code 3, i.e. a machine software interrupt, exactly
  the delivery the CLINT msip bit is specified to produce. Read from the
  CSR in the handler, not inferred.
- `traps-after-quiet` unchanged at 1 then 2: after the handler clears
  msip, no second trap arrives within the 2M-spin window, so clearing
  msip truly deasserts the source; it is not edge-retriggered or
  latched elsewhere.
- `spins = 0`: the trap had already fired by the time the wait loop
  first polled; delivery is immediate once MIE is re-enabled.
- `mepc` values differ between cycles because they point at the two
  different wait loops in `main`; both are ordinary M-mode instruction
  addresses in the program image.
- Latencies (about 52-55 us for cycle 1, 18-23 us for cycle 2 at
  1.49e9 units/s) vary with host load; the invariant that transfers is
  the register sequence (set -> mcause=3 -> clear -> quiet), re-checked
  by the program on every run.

## One defect found and fixed during development

Caught by QEMU's interrupt trace (`-d int`), not by reasoning.

1. 64-bit access to msip faults. The first version declared msip as a
   64-bit pointer; the very first read trapped with a load access
   fault (cause 5, tval 0x02000000), and the faulting load retried
   forever in the handler, hanging the program. QEMU 8.2.2's
   `sifive_clint` model only implements 4-byte accesses to the msip
   registers. Fixed by declaring msip as `volatile unsigned int *`;
   all accesses are now 32-bit, matching the CLINT register width.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's CLINT model on the `virt` machine, not
  real silicon. The msip set/clear delivery contract is architectural,
  but the 4-byte-only access width is this model's implementation
  detail, verified empirically above.
- Only hart 0, only M-mode, only the software interrupt. Timer
  interrupts, S-mode delegation of the software interrupt
  (`mideleg`), and multi-hart IPIs are not tested; the module is
  deliberately that small.
- The latency numbers are host-time costs of emulated trap delivery
  on the machine that ran QEMU (about 1.49e9 `rdcycle` units/s,
  calibrated per run against the 10 MHz `mtime`), not guest
  instruction counts. They will differ on other hosts; the invariant
  that transfers is the set -> mcause=3 -> clear -> quiet sequence,
  re-checked by the program on every run.
- The quiet windows (1M/2M spins) bound how long "no re-delivery" is
  observed; they are finite by construction (a bare-metal image never
  exits QEMU, so every wait needs a budget). A spurious re-delivery
  would have shown up as a counter change inside those windows.

## Reproduction

```
make msip.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel msip.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; QEMU is terminated by `timeout` afterwards
because the bare-metal image never exits QEMU on its own).
