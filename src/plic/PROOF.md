<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: PLIC claim/complete round-trip (backlog item 32)

## What was built

`src/plic/`: a bare-metal RISC-V program that drives the
platform-level interrupt controller on the QEMU `virt` board directly
through its memory-mapped registers, exercising exactly one mechanism:
the claim/complete round trip for a single interrupt source. Two
files, about 330 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos. No trap handler is installed and
`mie`/`mstatus` are never touched; the PLIC registers are polled, so
nothing outside the claim/complete path is under test.

- `plic_main.c`: UART bring-up, `rdcycle`/`mtime` clock calibration,
  PLIC programming with readback checks, interrupt assertion via UART
  loopback, the timed claim, the timed complete, and the PASS/FAIL
  verdict. Every check compares a register value against an expected
  constant; nothing is eyeballed.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make plic.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel plic.elf`
(or `make run-plic`).

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- PLIC context 0 (hart 0's M-mode context), base `0x0c000000`:
  - priority[10] at `0x0c000028` = 1
  - enable word 0 at `0x0c002000`, bit 10 set
  - threshold at `0x0c200000` = 0
  - claim/complete at `0x0c200004`
  - pending word 0 at `0x0c001000`, bit 10 observed
- Interrupt source 10 = UART0 on the `virt` machine.
- The interrupt is asserted without any external stimulus: the UART is
  put in internal loopback mode (MCR bit 4), its receive-data interrupt
  is enabled (IER bit 0), and one byte (`'Q'`, 0x51) is written to the
  transmitter holding register. The byte loops back into the receiver,
  the UART raises its IRQ line, and the PLIC sets pending bit 10.

## Controls (what the claim is compared against)

1. Before any PLIC programming, a claim returns 0: proves the claim
   register reads idle when nothing is pending, so the later 10 comes
   from the programmed state and not from reset values.
2. Every PLIC write is read back (priority[10]=1, enable bit 10=1,
   threshold=0): proves the programming took effect.
3. The looped-back byte is read out of the receiver and must equal
   0x51: ground truth that the assertion path (TX -> loopback -> RX ->
   IRQ) actually carried the byte, independent of the PLIC registers.

## Clock facts used (all measured, none assumed)

`rdcycle` on this QEMU does not count guest instructions and does not
run at the 10 MHz `mtime` rate. The program calibrates it on every boot
against the CLINT `mtime` (same construction as `src/wfi-latency/`):
1,000,000 mtime ticks (100 ms of virtual time) against the `rdcycle`
delta. All three runs measured ratio 149 `rdcycle` units per tick,
i.e. about 1.5e9 units/second, host-clock driven. The claim/complete
cycle counts below are therefore host-time costs of an emulated MMIO
round trip (vCPU exit, device emulation, re-entry), not guest
instruction counts.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

| step | run1 | run2 | run3 |
|---|---|---|---|
| `rdcycle`/`mtime` ratio | 149 | 149 | 149 |
| control claim (expect 0) | 0 | 0 | 0 |
| priority[10] / enable10 / thresh | 1 / 1 / 0 | 1 / 1 / 0 | 1 / 1 / 0 |
| pending bit 10 after TX byte (spins) | 1 (0) | 1 (0) | 1 (0) |
| claim id | 10 | 10 | 10 |
| claim cycles | 32310 | 29655 | 30885 |
| pending bit 10 immediately after claim | 0 | 0 | 0 |
| looped-back byte | 0x51 | 0x51 | 0x51 |
| pending bit 10 after source deasserted | 0 | 0 | 0 |
| complete cycles | 34260 | 40230 | 29730 |
| claim after complete (expect 0) | 0 | 0 | 0 |
| RESULT | PASS | PASS | PASS |

Identical logical results on all three runs; the only variation is the
host-time cycle counts (about 20-27 us per MMIO round trip at
1.5e9 units/s), which move with host load.

What each value means:

- `claim id = 10`: the claim register returned the UART source number,
  the highest-priority pending and enabled source above the threshold.
  This is the mechanism under test, verified against the programmed
  register state.
- `pending-after = 0` immediately after the claim: the claim cleared
  the pending bit (read before any further UART activity; see defect 3
  below for why the timing of this read matters).
- `pending-after-deassert = 0`: after the looped-back byte is read out
  of the receiver, the UART's IRQ line drops, and the pending bit
  follows it to 0.
- `reclaim = 0`: after writing 10 to the claim/complete register, a
  second claim returns 0. The source went claimed -> completed -> idle
  with no re-pend, because its input line was already deasserted.
- `spins = 0`: the pending bit was already set on the first poll
  iteration; the loopback assert path is synchronous in the UART model.

## QEMU model behavior this run depends on (read before porting)

Verified against the QEMU 8.2.2 source (`hw/intc/sifive_plic.c`), not
assumed:

- The pending bit follows the source's input level:
  `sifive_plic_irq_request` sets/clears the pending bit on every input
  line change. Dropping the UART IRQ line before the claim erases the
  pending state, so the test keeps the line asserted (IER_RDI enabled,
  received byte unread) until the claim is done.
- A claim read returns the highest-priority pending, enabled,
  not-yet-claimed source, clears its pending bit, and marks it claimed
  (the `claimed` bitmap is what suppresses a second claim, not the
  pending bit).
- A complete write clears the `claimed` bit. If the source input were
  still high at complete time, the model would re-pend it; the test
  reads the receiver first so the line is low, and the post-complete
  claim returns 0.

## Three defects found and fixed during development

All three were caught by the program's own checks or by missing output,
not by reasoning.

1. Loopback swallows console output. In loopback mode the UART model
   routes transmitted bytes to its own receiver instead of the host
   console, so every status line printed while loopback was enabled
   vanished (the first runs showed only the pre-loopback lines, then
   `RESULT`). Fixed by switching loopback back off immediately after
   the pending bit is observed, before any printing; the received byte
   is already in the receiver, so the IRQ line stays asserted.

2. Pending cleared on falling input edge. The first working version
   disabled the UART interrupt right after observing the pending bit,
   and the claim then returned 0: this QEMU's PLIC clears the pending
   bit when the source line falls. Fixed by keeping IER_RDI enabled
   (IRQ line asserted) through the claim, restoring IER only at the
   end.

3. Pending re-set by transmit activity. With the line held high, the
   pending bit read 1 after the claim (claim returned the correct 10):
   UART transmit activity between the claim and the pending read
   re-evaluated the still-asserted IRQ line and set the bit again.
   Fixed by reading the pending register immediately after the claim,
   before any further UART output. The `pending-after=0` values in the
   table are from that immediate read.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's PLIC model on the `virt` machine, not
  real silicon. The claim/complete register contract (claim returns
  the source id, complete re-arms it) is architectural, but the
  pending-bit-follows-input-level behavior is this model's
  implementation detail, documented above from its source.
- Only source 10 (UART0), only context 0 (hart 0 M-mode), only
  priority 1 vs threshold 0. Priority arbitration between multiple
  pending sources, threshold masking, and S-mode contexts are not
  tested; the module is deliberately that small.
- The cycle counts are host-time MMIO round-trip costs on the machine
  that ran QEMU (about 1.5e9 `rdcycle` units/s, calibrated per run
  against the 10 MHz `mtime`), not guest instruction counts. They will
  differ on other hosts; the invariant that transfers is the
  claim/complete register sequence, re-checked by the program on every
  run.
- No actual CPU interrupt is taken (`mie`/`mstatus` untouched); the
  test exercises the PLIC's claim/complete registers by polling, which
  is the whole of the mechanism under test.

## Reproduction

```
make plic.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel plic.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; QEMU is terminated by `timeout` afterwards
because the bare-metal image never exits QEMU on its own).
