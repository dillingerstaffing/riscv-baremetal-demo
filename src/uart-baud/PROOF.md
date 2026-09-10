<!-- PROOF-HEADER
Environment: QEMU 8.2.2
-->

# Proof: cycle-accurate UART baud check (backlog item #14)

## What was built

`src/uart-baud/baud_main.c`: a bare-metal RISC-V program that programs the
ns16550a divisor latch and measures the bit timing the UART model actually
produces, using the UART's own receive FIFO timeout as the time reference.

Build: `make uart-baud.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel uart-baud.elf`
(or `make run-uart-baud`).

One small mechanism, fully reviewable: about 300 lines, no dependencies
beyond `src/uart.c` and `src/boot.S`.

## Measurement definition

For each divisor D in {1, 12, 96}:

1. Program the divisor latch (DLAB dance: LCR=0x83, DLL/DLM, LCR=0x03, 8N1).
2. Enable the 16550A FIFOs (FCR), switch to internal loopback (MCR_LOOP),
   and set IER_RDI.
3. Write one byte to THR. It loops back into the receive FIFO, which arms
   QEMU's receive FIFO timeout for exactly 4 character times
   (40 bit times at 8N1: 1 start + 8 data + 1 stop).
4. Stamp `rdcycle` before the THR write; poll IIR until it reads 0x0C
   (character timeout indication); stamp `rdcycle` again.
5. Drain the byte outside the timed window. Repeat 12 times per divisor.

The poll loop has a deadline of 20x the expected timeout; a miss prints
`TIMEOUT`. Zero timeouts occurred in the captured runs.

`rdcycle` deltas are converted to picoseconds with a calibration of
`rdcycle` against the CLINT `mtime` over a 400 ms window (see clock notes).
Measured bit time = interval / 40, compared against the nominal bit time
for the programmed divisor.

## Why THRE/TEMT were rejected

The first idea was to time THR-empty / transmitter-empty. A direct probe
showed both bits already set microseconds after a THR write
(LSR=0x61 sampled right after the write, at every divisor). On this QEMU
the transmitter completes synchronously inside the THR MMIO write
(`serial_xmit` runs in the write path), so THRE/TEMT carry no baud timing
here. The program records this LSR sample (`sync-xmit LSR=0x61`) as
evidence instead of relying on those bits.

## Why IER_RDI is set

The 16550 reports the character timeout through the received-data
interrupt path, so IIR only shows 0x0C when the RDA interrupt is enabled.
Verified empirically: with IER=0 the timeout never appears in IIR
(all 36 runs TIMEOUT); with IER_RDI set, all 36 runs observe it.

## Clock facts used (all measured, none assumed)

- The virt device tree gives `/cpus/timebase-frequency = 10000000`: the
  CLINT `mtime` (and `rdtime`) runs at 10 MHz. The program cross-checks
  `rdtime` between two `mtime` reads on every run.
- `rdcycle` is NOT 10 MHz on this QEMU. Calibrated against `mtime`:
  run1 1497851133, run2 1497808822, run3 1498407649 units/second
  (repeatability about 0.02% with the 400 ms window).
- The virt board wires the ns16550a model with baudbase 399193
  (`hw/riscv/virt.c` passes 399193 to `serial_mm_init`), so the model's
  nominal bit time is divisor / 399193 seconds. The textbook 1.8432 MHz
  figure from the task brief does not describe this QEMU model; this
  experiment tests the model's actual law, divisor / 399193 s.
- Nominal bit times checked by the program: D=1: 2505053 ps,
  D=12: 30060647 ps, D=96: 240485178 ps
  (each = D * 10^12 / 399193, integer truncation; independently
  recomputed in Python during verification).

## Results (three independent QEMU runs, full logs in `bench-logs/`)

Minimum over 12 runs is the estimator: every host-side error source
(timer delivery stall, MMIO exit cost) is positive-only, so the minimum
is closest to the model's true timeout.

| divisor | nominal bit (ps) | min measured (ps) | min ppm | max ppm | mean ppm |
|---|---|---|---|---|---|
| 1 | 2505053 | 2921685 / 2907497 / 2957889 | +166316 / +160652 / +180769 | +1034508 / +547237 / +503260 | +374602 / +226415 / +242284 |
| 12 | 30060647 | 30581560 / 30576415 / 30744887 | +17328 / +17157 / +22761 | +1462723 / +40194 / +39995 | +327729 / +22749 / +26986 |
| 96 | 240485178 | 241128184 / 241299737 / 241489857 | +2673 / +3387 / +4177 | +56636 / +23095 / +21901 | +13728 / +6175 / +7060 |

(three values per cell: run1 / run2 / run3)

Key observations:

- At divisor 96, the closest measurement is within +0.27% to +0.42% of
  the nominal divisor/399193 bit time across three independent runs.
- The ppm error SHRINKS as the divisor grows (about 17% at D=1,
  about 2% at D=12, about 0.3% at D=96). That is the signature of an
  additive host latency on top of a correct 1/D law, not a multiplicative
  baud error: if the model's baud were wrong by some factor, the ppm
  would be constant across divisors. It is not.
- `bit/divisor` computed from the per-divisor minima converges toward the
  baudbase-implied 2505053 ps as the timeout lengthens (my own analysis of
  the logged minima, not the program's mean-based print):
  run1: 2921685 / 2548463 / 2511751; run2: 2907497 / 2548034 / 2513538;
  run3: 2957889 / 2562073 / 2515519.
- The residual excess at D=96 (about 26-40 us per 40-bit window) matches
  the independently measured one-off costs inside each run: one THR write
  (min 3555-20910 cycles across runs, i.e. a few microseconds, host
  dependent) plus one poll iteration (LSR read min 1320 cycles), plus the
  main-loop thread's virtual-timer delivery latency on this shared VM.

## Limits of verification (read before citing numbers)

- This measures the QEMU 8.2.2 `virt` UART model, not real hardware and
  not any other QEMU version. The baudbase 399193 is specific to how this
  QEMU wires the ns16550a.
- Host scheduling: QEMU's virtual-clock timers are processed by the
  main-loop thread, so on a loaded host the timeout can be delivered late
  by microseconds to milliseconds. All such delays are positive; they
  appear as the max outliers (e.g. +146% at D=12 in run1) and never as
  negative error. The minimum over runs is robust to them; the mean is not.
- Absolute accuracy is bounded by the `rdcycle` calibration repeatability
  (about 0.02% across runs with the 400 ms window) plus the one-off
  per-run costs above. Claims finer than about 0.5% would overstate what
  was measured.
- Divisor 1 is dominated by host latency (100 us timeout vs ~13 us
  latency); divisor 96 is the precision point.
- `rdcycle` on this QEMU is about 1.5e9 units/second, not 10 MHz and not
  1.8432 MHz; the brief's oscillator assumption was checked and does not
  apply to this model.
- The sync-xmit check (LSR=0x61 immediately after THR write) is evidence
  about this QEMU's transmitter model only.

## Reproduction

```
make uart-baud.elf
timeout 60 qemu-system-riscv64 -machine virt -nographic -bios none -kernel uart-baud.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs: `bench-logs/run1.log`,
`bench-logs/run2.log`, `bench-logs/run3.log` (each ends with `done`; QEMU
is terminated by `timeout` afterwards because the bare-metal image never
exits QEMU on its own).

Every number in the table above was re-verified from the logs with an
independent Python pass: nominals recomputed as D*10^12//399193, each
per-run ppm recomputed with C truncation-toward-zero semantics, and
min/max/mean/bit-divisor/sync-LSR all cross-checked.
