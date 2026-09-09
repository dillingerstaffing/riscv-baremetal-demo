# PROOF: rdcycle monotonicity over a 100-nop window (backlog item 77)

Backlog item 77: in M-mode on QEMU, read `rdcycle` around a fixed
100-nop sequence and verify the delta is positive and monotonic
across 1000 consecutive reads; publish min/median/max deltas across
3 runs. This is the cycle-counter ground truth the other benchmark
modules stand on.

## What was built

`src/cycmon/cyc_main.c`, a bare-metal M-mode binary sharing only
`src/boot.S` and the UART driver with the other demos. It:

1. Takes 1000 samples. Each sample is one volatile asm block
   holding `csrr cycle`, exactly 100 `nop` instructions, and
   `csrr cycle` again, so the -O2 compiler cannot insert or reorder
   anything between the two reads. It records the before-read and
   the delta per sample.
2. Computes and checks three invariants (see below), then prints
   `RESULT: PASS` or `RESULT: FAIL`.
3. Encodes the verdict in the program exit path: on PASS it writes
   the virt test-device finisher word `0x5555` at `0x100000`, which
   shuts the machine down (QEMU exits 0); on FAIL it parks the hart
   in a `wfi` loop without touching the finisher, so under the
   harness's `timeout` a FAIL is observable as exit status 124 as
   well as the `RESULT: FAIL` line.

Build integration: `Makefile` gains `cycmon.elf`, `run-cycmon`,
and the module is in `all` and `clean`.

The 100-nop window is verified in the shipped binary, not just the
source: disassembling `cycmon.elf` shows `rdcycle a2`, then exactly
100 `nop` instructions (`0001` encodings, addresses
`0x80000254`-`0x8000031a`), then `rdcycle a4`, with zero
non-`nop` instructions between the two reads. The sampler is
inlined once into the measurement loop; the whole binary contains
exactly 100 `nop` instructions.

## Why the counter must behave this way (source verification, QEMU 8.2.2)

`target/riscv/csr.c`: `CSR_CYCLE` dispatches to `read_hpmcounter`
with counter index 0, which calls `riscv_pmu_read_ctr` and returns
`get_ticks() - ctr_prev + ctr_val`. `get_ticks()` is
`cpu_get_host_ticks()`, `rdtsc` on x86_64. So `rdcycle` samples the
host tick counter: "cycle" units are host ticks, not guest
instructions, and the 100-nop delta is the host time QEMU's TCG
spends on 100 translated nops plus two emulated CSR reads.
Monotonicity of successive reads is the property under test; the
absolute delta values are host properties, reported, not gated.

The virt board always instantiates the `sifive,test` finisher device
at `0x100000` (`VIRT_TEST`). Measured on this build: writing
`0x5555` shuts the machine down and the QEMU process exits 0 within
tens of milliseconds. Writing `0x3333` also shuts down with exit 0
on QEMU 8.2.2 (no distinct fail exit code), which is why the FAIL
path parks the hart instead of writing the fail word.

## Checks and invariants (all computed, none eyeballed)

1. Every delta strictly positive, and below 2^40 (a backward
   counter would appear as a huge unsigned value).
2. Reads monotonic across samples: for every i >= 1,
   `c0[i] - c0[i-1] >= delta[i-1]`, i.e. the start read of sample i
   is at or after the end read of sample i-1. Reported as a count
   of backward reads (must be 0).
3. Deltas bounded: at most 1% of samples may exceed 2^20 ticks
   (~0.9 ms at the observed host tick rate). A stuck counter would
   fail check 1; a runaway counter would exceed the threshold on
   most samples. The allowance absorbs rare host scheduling stalls.
   Outliers are counted and reported, not hidden.

Check 3 took its final form from a measurement. The first build
gated on an absolute `max <= 2^20`, and one run tripped it: a
single delta of 2,309,850 ticks (~1.9 ms at the host tick rate)
during a run whose mtime window also stretched to 18411 ticks, a
host scheduling stall, not a counter malfunction (all deltas
positive, 0 backward reads). An absolute bound cannot distinguish a
host stall from a broken counter on a shared host, so the check
became the 1% outlier allowance, the same construction the
counter-alias module uses for host jitter.

## Build

Toolchain: `riscv64-unknown-elf-gcc` (xPack RISC-V GCC 15.2.0,
invoked under its `riscv64-unknown-elf-` compatibility name),
QEMU 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18).

Exact build command (`make cycmon.elf`), full output in
`src/cycmon/bench-logs/build.log`:

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/cycmon/cyc_main.c -o src/cycmon/cyc_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o cycmon.elf src/boot.o src/uart.o src/cycmon/cyc_main.o
.../riscv-none-elf/bin/ld: warning: cycmon.elf has a LOAD segment with RWX permissions
```

(The RWX linker note is benign and appears for every module built
with this link script.)

Each run: `timeout 60 qemu-system-riscv64 -machine virt -nographic
-bios none -kernel cycmon.elf`, raw UART output preserved in
`src/cycmon/bench-logs/run1.log`, `run2.log`, `run3.log`, each with
the harness-appended QEMU exit status.

## Measured results, 2026-09-09

Run 1:
- 1000 samples, run window 2714 mtime ticks.
- Delta (after - before): min=135, median=150, max=34620,
  outliers above 2^20: 0.
- Backward reads across samples: 0. RESULT: PASS, qemu exit 0.

Run 2:
- 1000 samples, run window 2844 mtime ticks.
- Delta: min=135, median=135, max=33735, outliers: 0.
- Backward reads: 0. RESULT: PASS, qemu exit 0.

Run 3:
- 1000 samples, run window 69358 mtime ticks (a host stall landed
  between samples, outside any delta window: ~6.9 ms of mtime while
  no single delta exceeded 48060 ticks).
- Delta: min=135, median=150, max=48060, outliers: 0.
- Backward reads: 0. RESULT: PASS, qemu exit 0.

Summary across runs: min/median/max = 135/150/34620,
135/135/33735, 135/150/48060 host ticks. Every delta strictly
positive in all 3000 samples; zero backward reads; zero outliers
above 2^20 in every run. The verdict line `RESULT: PASS` and the
qemu exit status 0 are identical across all three runs. Full raw
outputs are in the bench-logs directory quoted above.

Exit-path verification: the earlier build's host-stall run printed
`RESULT: FAIL` and the QEMU process, parked in `wfi` without
touching the finisher, was terminated by the harness timeout
(exit 124, with QEMU's own "terminating on signal 15" line in the
log). PASS runs shut down via the finisher word and exit 0. The
exit-path code is identical in the shipped build.

## Raw run logs

run1.log:
```
cycmon: rdcycle monotonicity over 100-nop window
samples: 1000
run window: 2714 mtime ticks
delta (after - before): min=135 median=150 max=34620 outliers(>2^20)=0
backward reads across samples: 0
RESULT: PASS
[harness] qemu exit status: 0
```

run2.log:
```
cycmon: rdcycle monotonicity over 100-nop window
samples: 1000
run window: 2844 mtime ticks
delta (after - before): min=135 median=135 max=33735 outliers(>2^20)=0
backward reads across samples: 0
RESULT: PASS
[harness] qemu exit status: 0
```

run3.log:
```
cycmon: rdcycle monotonicity over 100-nop window
samples: 1000
run window: 69358 mtime ticks
delta (after - before): min=135 median=150 max=48060 outliers(>2^20)=0
backward reads across samples: 0
RESULT: PASS
[harness] qemu exit status: 0
```

## Emulator and host-jitter limits, honestly

- "Cycle" units are host ticks (`rdtsc`), not guest instructions.
  The 100-nop delta of 135-150 ticks typical is the host time for
  two emulated CSR reads plus 100 TCG-translated nops; it is a
  property of this host and this QEMU build, not of the guest code.
- This host is a shared AMD EPYC 9D64 VM, so scheduling stalls of
  milliseconds happen (seen in run 3's stretched window and in the
  earlier build's tripped absolute bound). The checks gate on what
  the module owns, positivity and monotonicity of the reads, and
  treat stalls as counted outliers.
- Median deltas of 135 vs 150 ticks between runs are host jitter
  in TCG translation caching, not a signal.
