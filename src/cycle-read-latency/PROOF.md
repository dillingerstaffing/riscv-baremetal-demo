<!-- PROOF-HEADER
Checks: 2964
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: rdcycle read-latency floor (backlog item 145)

Backlog item 145: in M-mode on QEMU, 1000 consecutive rdcycle reads
with no intervening instructions between reads, publishing the
min/median/max delta between successive reads as the read-latency
floor of the counter itself; publish the deltas and a small
histogram across 3 runs. Verify: run on QEMU, publish values.

## What was built

`src/cycle-read-latency/crl_main.c` plus a minimal M-mode trap
handler `src/cycle-read-latency/crl_trap.S`, sharing only
`src/boot.S` and the UART driver with the other demos. It:

1. Installs the trap handler (mtvec direct, mscratch pointing at
   the trap record area) and runs an rdcycle-vs-mtime calibration
   over a 10000-mtime-tick window, publishing cycles-per-mtime-tick
   so the deltas below are interpretable.
2. Takes 38 bursts of 27 back-to-back `csrr x, cycle` reads = 1026
   reads. Each burst is a single volatile asm block writing into 27
   distinct registers (t0-t6, a0-a7, s0-s11; x1/ra deliberately not
   used), so the -O2 compiler emits the 27 reads verbatim with zero
   instructions between them. The values are stored to memory after
   the burst.
3. Computes deltas only over the 26 consecutive pairs inside each
   burst (988 deltas). The 37 boundary intervals contain 27 stores
   plus loop bookkeeping, so they are excluded by construction, not
   hidden. Then it publishes min/median/max, a 16-bin histogram, and
   `RESULT: PASS` or `RESULT: FAIL`.
4. Encodes the verdict in the program exit path: on PASS it writes
   the virt test-device finisher word `0x5555` at `0x100000`, which
   shuts the machine down (QEMU exits 0); on FAIL it parks the hart
   in a `wfi` loop without touching the finisher, so under the
   harness's `timeout` a FAIL is observable as exit status 124 as
   well as the `RESULT: FAIL` line. Any trap records
   mcause/mepc/mtval and parks the hart (no RESULT line at all),
   so the PASS verdict also covers "zero traps fired".

Build integration: `Makefile` gains `cycle-read-latency.elf`,
`run-cycle-read-latency`, and the module is in `all` and `clean`.

The back-to-back construction is verified in the shipped binary,
not just the source: disassembling `cycle-read-latency.elf` shows
the inlined burst at `0x800002e4`-`0x8000034c` is exactly 27
consecutive `rdcycle` instructions (`c0002xx3` encodings) with zero
non-`rdcycle` instructions between the first and the last, and the
binary's longest consecutive `rdcycle` run anywhere is 27. (The two
other `rdcycle` instructions in the binary are the calibration
pair.)

## Why the counter must behave this way (source verification, QEMU 8.2.2)

`target/riscv/csr.c`: `CSR_CYCLE` dispatches to `read_hpmcounter`
with counter index 0, which calls `riscv_pmu_read_ctr` and returns
`get_ticks() - ctr_prev + ctr_val`. `get_ticks()` is
`cpu_get_host_ticks()`, `rdtsc` on x86_64. So `rdcycle` samples the
host tick counter: "cycle" units are host ticks, not guest
instructions, and the delta between two back-to-back reads is the
host time for one emulated CSR read, i.e. the read-latency floor of
the counter itself. Positivity of the deltas is the property under
test; the absolute values are host properties, reported, not gated.

The virt board always instantiates the `sifive,test` finisher device
at `0x100000` (`VIRT_TEST`). Measured on this build: writing
`0x5555` shuts the machine down and the QEMU process exits 0.

## Checks (all computed, none eyeballed)

1. Every one of the 988 deltas strictly positive, and below 2^40 (a
   backward counter would appear as a huge unsigned value).
2. Zero traps across the run (the trap count is printed and must
   be 0; a trap parks the hart before any RESULT line is printed).

A stuck counter would fail check 1 on every delta; a backward
counter would fail it via the 2^40 cap.

## Build

Toolchain: `riscv64-unknown-elf-gcc` (xPack RISC-V GCC 15.2.0,
invoked under its `riscv64-unknown-elf-` compatibility name),
QEMU 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18).

Exact build command (`make cycle-read-latency.elf`), full output in
`src/cycle-read-latency/bench-logs/build.log`:

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/cycle-read-latency/crl_trap.S -o src/cycle-read-latency/crl_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/cycle-read-latency/crl_main.c -o src/cycle-read-latency/crl_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o cycle-read-latency.elf src/boot.o src/uart.o src/cycle-read-latency/crl_trap.o src/cycle-read-latency/crl_main.o
.../riscv-none-elf/bin/ld: warning: cycle-read-latency.elf has a LOAD segment with RWX permissions
```

(The RWX linker note is benign and appears for every module built
with this link script.)

Each run: `timeout 60 qemu-system-riscv64 -machine virt -nographic
-bios none -kernel cycle-read-latency.elf`, raw UART output
preserved in `src/cycle-read-latency/bench-logs/run1.log`,
`run2.log`, `run3.log`, each with the harness-appended QEMU exit
status.

## Measured results, 2026-09-10

Run 1:
- 1026 reads in 38 bursts of 27; 988 deltas.
- Calibration: 1552530 cycles over 10001 mtime ticks = 155 +
  2375/10001 cycles per mtime tick.
- Delta: min=135, median=165, max=222525 host ticks.
- Histogram (width 13908): bin 0 holds 987 deltas, bin 15 holds 1
  (the 222525-tick stall outlier). Bad deltas: 0. Traps: 0.
- RESULT: PASS, qemu exit 0.

Run 2:
- Calibration: 1548615 cycles over 10000 mtime ticks = 154 +
  8615/10000 cycles per mtime tick.
- Delta: min=135, median=180, max=1530195 host ticks.
- Histogram (width 95638): bin 0 holds 987 deltas, bin 15 holds 1
  (a ~10 ms host stall landing inside one read pair). Bad deltas:
  0. Traps: 0.
- RESULT: PASS, qemu exit 0.

Run 3:
- Calibration: 4069755 cycles over 26782 mtime ticks = 151 +
  25673/26782 cycles per mtime tick (a host stall stretched the
  calibration window itself; the ratio is stall-tolerant because it
  divides totals).
- Delta: min=135, median=165, max=9810 host ticks.
- Histogram (width 614): bin 0 [0..613] holds 962 deltas, with the
  remaining 26 spread across bins 9-15 (5526..9823 ticks). Bad
  deltas: 0. Traps: 0.
- RESULT: PASS, qemu exit 0.

Summary across runs: min/median/max = 135/165/222525,
135/180/1530195, 135/165/9810 host ticks; rdcycle-vs-mtime
calibration 155, 154, 151 cycles per mtime tick. Every delta
strictly positive in all 2964 samples; zero bad deltas; zero traps.
The verdict line `RESULT: PASS` and the qemu exit status 0 are
identical across all three runs. Full raw outputs are in the
bench-logs directory quoted above.

## Reading the numbers, honestly

- The min delta of 135 ticks is identical in all 3 runs and matches
  the 135-tick floor measured independently by the cycmon module's
  100-nop windows on this host: one emulated CSR read costs ~135
  host ticks at minimum. The median of 165-180 ticks is the typical
  back-to-back read cost including TCG translation-cache effects.
- The max values (9810 to 1530195 ticks) are host scheduling stalls
  on this shared VM, not read latencies: run 2's 1.53M-tick max is
  ~10 ms of host time inside a single read pair. That is why the
  checks gate on positivity and the 2^40 cap, not on any absolute
  bound; the histograms show the tail honestly.
- "Cycle" units are host ticks (rdtsc), not guest instructions.
  The calibration ratio of ~151-155 cycles per mtime tick is the
  host clock rate divided by the virt board's 10 MHz mtime on this
  machine, reported per run, not assumed.
