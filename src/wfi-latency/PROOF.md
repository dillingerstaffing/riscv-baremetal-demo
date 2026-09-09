# Proof: WFI wakeup latency (backlog item 28)

## What was built

`src/wfi-latency/`: a bare-metal RISC-V program that arms the CLINT
timer 20000 mtime ticks (2 ms) ahead, then either executes `wfi` or spins on a
flag, and measures the latency from the programmed wakeup to the first
instruction executed after the trap returns. 400 trials of each kind,
interleaved, first 8 of each discarded from statistics. Four files,
about 700 lines total, sharing only `src/boot.S`, `src/uart.c`, and
`src/preempt/clint.c` with the other demos.

- `wfi_main.c`: UART bring-up, trap vector install, clock calibration,
  the two trial functions (exact asm blocks, see below), the trial
  loop, statistics (min/p50/max/mean, histograms), and the in-program
  verification checks with the PASS/FAIL verdict.
- `wfi_trap.S`: M-mode trap entry. Stamps `rdtime` as the 3rd
  instruction and `rdcycle` as the 5th, saves all registers, runs the C
  handler on a dedicated trap stack, restores everything, `mret`.
- `wfi.h`: the trap save-area layout (must match the asm offsets) and
  the per-trial record.
- `PROOF.md` (this file), `bench-logs/` with the build log, three raw
  QEMU run logs, and a clock-calibration log.

Build: `make wfi-latency.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel wfi-latency.elf`
(or `make run-wfi-latency`).

## The two clocks on this QEMU build (measured, not assumed)

The experiment's first finding is about the counters themselves, and
it changed the measurement design. Two throwaway probes plus the
module's own calibration established:

- `mtime` (CLINT, 0x0200bff8) advances at the device-tree 10 MHz rate
  while the QEMU process runs. Verified against the host clock:
  10,001,075 ticks elapsed in 0.982 s of vcpu CPU time (1.078 s wall),
  i.e. 10.19 MHz by CPU time (`bench-logs/clock-calibration.log`).
- `rdcycle` does NOT advance at 100 units per tick. On QEMU 8.2.2
  without `-icount`, `rdcycle` returns `cpu_get_host_ticks()`, and on
  this host that counter runs at the host CPU rate: the measured
  rdcycle/mtime ratio is 149.75 to 149.78, stable to about 0.1% across
  runs, and `/proc/cpuinfo` reports the host CPU at 1497.750 MHz.
  149.78 ticks * 10 MHz = 1497.8 MHz. So one rdcycle unit is one host
  CPU cycle (0.667 ns), not one nanosecond.

Consequence: subtracting an `mtimecmp` value (ticks) from an `rdcycle`
reading mixes two different clocks and is meaningless here. The
latency metrics below are therefore computed strictly inside one clock
domain: tick-domain metrics use only `rdtime` stamps, rdcycle-domain
metrics use only `rdcycle` stamps. The module re-checks the 149.75
ratio on every boot and fails the run if it leaves the 145..155 band.

## Method

Each trial programs `mtimecmp = mtime + 20000` ticks (2 ms at the
10 MHz timebase) with only the machine timer interrupt enabled in
`mie`, then runs one exact asm block. `mstatus.MIE` is 0 everywhere
except inside the trial block, so a trap can only land in trial code.
The 2 ms arming distance was chosen after a 2000-tick version showed
that ordinary host descheduling of the QEMU vcpu can occasionally
stretch the arm-to-trial window past a 200 us deadline. The final code
handles this directly: after each trial it checks `t_pre < cmp`, and if
the host stretched the window (timer already pending at trial start),
it re-arms and re-runs that trial, up to 100 attempts. Only trials with
a clean arm are recorded; the retry count is printed and the expected
trap count is adjusted accordingly (`800 + retries`). The in-program
check `t_pre < cmp` remains as an invariant.

WFI trial block (emitted verbatim, no compiler reordering possible):

```
rdtime  t_pre        # tick stamp before enabling interrupts
rdcycle c_pre        # rdcycle stamp
csrsi   mstatus, 8   # MIE = 1
wfi
wfi_resume:          # global label; the handler resumes here
rdcycle c_resume     # first instruction after mret
rdtime  t_resume     # second instruction after mret
```

Spin baseline block: identical, but between `csrsi` and the resume
label the hart spins on `lw t0,0(flag); beqz t0,1b` instead of `wfi`.
The trap handler sets the flag and likewise resumes at `spin_resume`,
so both kinds stamp the resume point at the same structural position:
the first instruction after `mret`.

Trap entry (`wfi_trap.S`): `csrrw` t0 with mscratch, park the
interrupted t1, `rdtime`, `rdcycle`, save x1-x31, record mcause/mepc,
then call the C handler. The handler requires
`mcause == 0x8000000000000007` (machine timer interrupt), records the
stamps into the trial record, disarms the timer (`mtimecmp = ~0` so
`mret` does not re-trap), and sets `mepc` to the resume label. Any
other cause is recorded and the run fails.

Per-trial metrics (tick domain unless noted):

- `wake_to_resume = t_resume - cmp`: programmed wake to first task
  instruction. The headline number; exact in the tick domain.
- `wake_to_entry = t_entry - cmp`: programmed wake to trap entry.
- `entry_to_resume = t_resume - t_entry`: the trap path in ticks.
- `trap_cyc = c_resume - c_entry`: the trap path in rdcycle units
  (fine-grained view; divide by about 149.75 for ticks).

In-program checks (any failure prints FAIL and the run reports FAIL):

- every trial trapped exactly once with mcause = machine timer
  interrupt (trap_count == 800 + arm_retries, per-trial mcause check);
- `t_pre < cmp` on every recorded trial (guaranteed by the re-arm
  retry; the check remains as an invariant);
- `t_entry >= cmp`, `t_resume >= t_entry`,
  `c_entry > c_pre`, `c_resume > c_entry` (stamp ordering);
- wfi trials: `mepc_before` equals the address of the `wfi` or the
  address after it, nothing else;
- spin trials: `mepc_before` inside the spin loop, nothing else.

## Calibration results (from the run logs)

On every boot the module spins for 10,000,000 mtime ticks and measures
the rdcycle delta, then checks 64 back-to-back (rdcycle, mtime) pairs
for monotonicity:

- run1: 1,497,811,560 rdcycle units (ratio 149.78), monotonic ok
- run2: 1,497,833,205 rdcycle units (ratio 149.78), monotonic ok
- run3: 1,500,384,480 rdcycle units (ratio 150.04), monotonic ok

A standalone probe (`bench-logs/clock-calibration.log`, built with the
same toolchain) spun for exactly 10,001,075 mtime ticks: rdcycle delta
1,497,916,755 (ratio 149.78), in 0.982 s of vcpu CPU time (1.078 s
wall). 10,001,075 ticks in 0.982 s is 10.19 MHz, consistent with the
device-tree 10 MHz timebase; the wall-clock gap is host overhead. The
ratio 149.78 matches the host CPU frequency (1497.751 MHz, from
`/proc/cpuinfo`, saved in `bench-logs/host-cpu.txt`) divided by 10 MHz
to 0.02%, confirming that on this QEMU build one rdcycle unit is one
host CPU cycle.

## Results

392 measured trials per kind per run (first 8 discarded). All runs:
800+retries timer traps, mcause 0x8000000000000007 every time, no
unexpected traps, RESULT: PASS. Times in mtime ticks (100 ns); rdcycle
in host CPU cycles.

| run | kind | wake_to_resume min/p50/max | wake_to_entry p50 | entry_to_resume p50 | trap_rdcycle p50 | arm retries |
|-----|------|---------------------------|-------------------|---------------------|------------------|-------------|
| 1 | wfi | 183 / 533 / 29407 | 489 | 41 | 5460 | 2 |
| 1 | spin | 145 / 307 / 107300 | 282 | 29 | 3780 | |
| 2 | wfi | 201 / 452 / 16758 | 422 | 23 | 3075 | 0 |
| 2 | spin | 159 / 312 / 24776 | 291 | 20 | 2670 | |
| 3 | wfi | 200 / 2017 / 355656 | 1971 | 55 | 7440 | 1 |
| 3 | spin | 144 / 1070 / 449419 | 1041 | 38 | 5145 | |

The wfi-vs-spin gap (the vcpu wakeup cost) per run, in ticks:

- run1: wake_to_resume p50 gap 226 (533 vs 307); wake_to_entry p50 gap 207
- run2: wake_to_resume p50 gap 140 (452 vs 312); wake_to_entry p50 gap 131
- run3: wake_to_resume p50 gap 947 (2017 vs 1070); wake_to_entry p50 gap 930

The trap path itself (entry_to_resume) is essentially identical for
both kinds in every run (p50 20-55 ticks), as designed: the only
difference between the two trial kinds is whether the hart was halted
in `wfi` or spinning when the timer fired.

## The mepc observation

In all three runs, all 400 wfi trials resumed with `mepc_before`
equal to the address after the `wfi` instruction (`after_wfi=400`,
`at_wfi=0`, `other=0`), and all 400 spin trials trapped inside the
spin loop (`other=0`). QEMU completes the `wfi` before delivering the
interrupt, which is the architecturally permitted behavior (wfi may be
implemented as a no-op). This confirms the trap was taken from the
intended location; it does not prove a physical low-power state, which
the guest cannot observe.

## What the numbers mean (and what they do not)

Within each run, the comparison is controlled: the only difference is
wfi vs spin. Waking a halted vcpu costs about 1.5 to 1.9x the
timer-interrupt latency of a spinning hart on this QEMU build, or
roughly 130 to 230 ticks (13 to 23 us) of extra vcpu-wakeup latency in
the quieter runs. The trap path (entry to first resumed instruction)
is the same either way.

The absolute latencies move with host load: run3's medians are about
4x run2's, and both saw multi-millisecond max outliers from host
descheduling. These are QEMU-on-a-shared-host measurements, not
silicon numbers. They do not characterize any physical core's wfi
power behavior; they characterize QEMU 8.2.2's vcpu halt/wake path on
this host, measured honestly.

## Limits of verification

- This verifies QEMU 8.2.2's virt machine on this host, not silicon.
  The `wfi` sleep state is whatever QEMU's vcpu halt does; the guest
  cannot observe a power state, only the timing.
- Tick-domain metrics have 100 ns quantization (one mtime tick).
- rdcycle-domain metrics are in host CPU cycles and include host
  scheduling effects; the median over 392 trials is robust to that,
  the max is not.
- Only hart 0, only M-mode, only the machine timer interrupt.

## Reproduction

```
make wfi-latency.elf CROSS=riscv-none-elf-
timeout 90 qemu-system-riscv64 -machine virt -nographic -bios none -kernel wfi-latency.elf
```

Toolchain used for the final binary: xPack `riscv-none-elf-gcc`
15.2.0, QEMU 8.2.2. The module was first built with distro
`riscv64-unknown-elf-gcc` 13.2.0 (build log preserved at
`bench-logs/build-13.2.0.log`); a VM environment restart during the
task removed that toolchain, so the final build and all run logs use
the xPack toolchain, which accepts the same flags and target options.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; QEMU is terminated by `timeout` afterwards
because the bare-metal image never exits QEMU on its own).
Clock calibration: `bench-logs/clock-calibration.log`.
Host CPU info: `bench-logs/host-cpu.txt`.

A fourth log, `bench-logs/run3_hostloaded_FAIL.log`, is kept on
purpose: it is a run taken while a compiler was running on the host,
and 11 trials failed the "interrupt already pending at trial start"
check because host descheduling stretched the arm-to-trial window past
the 2000-tick arming distance. A fifth,
`bench-logs/run1_ambient_desched_FAIL.log`, shows a single trial
failing the same check with no deliberate host load. Both runs reported
FAIL honestly and are not counted in the results; they document that
the check works. They are why the final code re-arms and retries a
trial whose arm window was stretched (instead of failing the run), and
why the arming distance is 20000 ticks (2 ms) instead of 2000.

## Defects found and fixed during development

The first version of the spin trial's inline asm used plain `=r`
outputs together with an `"r"` input (`&wfi_spin_flag`) that is read
late (inside the spin loop). GCC allocated an output and the input to
the same register, so `lw t0,0(a5)` loaded from a time value instead
of the flag address and every spin trial took a load access fault
(mcause 0x5). Fixed with early-clobber `=&r` on the outputs; the
disassembly was re-inspected to confirm the input keeps its own
register. The wfi trials were unaffected (no input operand).

The trap entry passed `t0` (pointing at the save area) into the C
handler, then used `t0` again afterwards. `t0` is caller-saved, so this
was an ABI violation even though the compiled handler happened not to
clobber it. Fixed by reloading `la t0, wfi_save` after the call;
verified in the disassembly.

The original design failed the whole run if host descheduling
stretched the arm-to-trial window past the arming distance (observed
once in 800 trials even with no deliberate host load). Fixed by
re-arming and re-running the affected trial (up to 100 attempts) and
accounting the retries in the expected trap count, so every recorded
trial has a clean arm.
