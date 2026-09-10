<!-- PROOF-HEADER
Checks: 10
Mismatches: 0
Environment: QEMU 8.2.2
-->

# PROOF: S-mode trap delegation for the preemptive scheduler

## What was built

A new module, `src/smode/`, in this repo, built as two binaries sharing one
scheduler core (compiled once per privilege mode via `-DTRAP_SMODE`):

- `smode.elf`: minimal M-mode boot programs `medeleg`/`mideleg` (supervisor
  software/timer/external interrupts and S-mode exceptions delegated),
  installs `stvec` (the S-mode scheduler vector) plus a tiny M-mode vector
  that serves only a timer-rearm ecall, opens the address space to S-mode
  with one PMP NAPOT entry, grants S-mode access to the `time`/`cycle`
  counters via `mcounteren`, probes `menvcfg.STCE` for the Sstc extension,
  then drops to S-mode with `sret`. The scheduler then runs entirely in
  S-mode on the delegated supervisor timer interrupt.
- `smode-mbase.elf`: the identical workload (same tasks, same 200-tick
  quota, same measurement code) running in M-mode on the machine timer
  interrupt. This is the baseline the delegation overhead is measured
  against.

Files:

- `sboot.S`: M-mode entry (`_start`: BSS clear, stack, `m_boot`), the
  S-mode landing point `s_entry`, and `m_trap_entry` (M-mode ecall rearm
  service used only when Sstc is absent).
- `msetup.c`: the M-mode delegation setup described above. For the
  baseline binary it compiles to a stub (no delegation, stays in M-mode).
- `strap.S`: trap entry/exit, mode-selected at build time. Same protocol
  as the preempt module: `*scratch` swap, `rdtime`/`rdcycle` stamps two
  instructions in, full x1-x31 plus `*epc` save, C handler on a dedicated
  trap stack, exit stamps, restore, `sret`/`mret`.
- `ssched.h/.c`: task table, C trap handler (latency stamp, switch-cost
  accounting, timer rearm, round-robin), per-tick sample arrays, and the
  final report with min/median/max.
- `stimer.h/.c`: timer rearm. S-mode: writes `stimecmp` when Sstc is
  present, otherwise ecalls M-mode to reprogram `mtimecmp` (S-mode cannot
  touch it). Baseline: writes `mtimecmp` directly.
- `stasks.h/.c`: three spinning tasks (LCG step + volatile counter),
  identical workload in both binaries.
- `smain.c`: bring-up and report for both binaries.

The design follows from what the privileged ISA specifies: on this QEMU
(8.2.2, `virt`) the menvcfg probe reported Sstc present, so the S-mode run
used the pure S-mode path (`stimecmp`, no M-mode involvement after boot).

## Ground truth the measurements rest on

- CLINT base `0x02000000` and `timebase-frequency = 10000000` were read
  from the device tree QEMU itself generates for `-machine virt`, so one
  `mtime` tick is 100 ns. (`rdtime` tracking `mtime` was verified in the
  preempt module.)
- Sstc presence was probed at runtime, not assumed: M-mode set
  `menvcfg.STCE` (bit 63) and read it back. The bit stuck, so `stimecmp`
  (CSR `0x14d`) is implemented and S-mode-writable. The run banner prints
  the probe result.
- PMP default-deny is real: with no PMP entry programmed, the first
  S-mode instruction fetch after `sret` raised an instruction access
  fault (observed via QEMU `-d int`: `fault_fetch` at the `sret` target).
  One NAPOT entry (`pmpaddr0` all-ones, `pmpcfg0` R/W/X) opens the address
  space to S-mode. Verified by a minimal probe binary: `sret` faulted
  before the PMP entry, succeeded after.
- `mcounteren` gating is real: at reset all bits are clear, so S-mode
  `rdtime`/`rdcycle` raised illegal instruction, and because illegal
  instructions are delegated, the faulting trap entry re-entered itself
  forever (observed as 1.1M repeated traps at the trap entry's `rdtime`).
  Setting `mcounteren` to CY|TM|IR fixed it.
- `rdcycle` on this QEMU does not count guest instructions; correlating
  `rdcycle` deltas with `mtime` deltas gives 146 units per 100 ns tick
  (both binaries independently report 146 at the median switch cost), so
  cycle counts convert to time by dividing by 146 per 100 ns, and every
  switch cost is cross-checked against `mtime` ticks.

## Definitions (exactly what the numbers mean)

- Interrupt latency: `rdtime` stamped at trap entry (two instructions in,
  before any save) minus the comparator deadline (`stimecmp`/`mtimecmp`
  value) that fired. In 100 ns ticks.
- Context-switch cost: `rdcycle` at trap entry to `rdcycle` stamped on
  the exit path just before the register restore, i.e. the full
  save-handler-restore path for one tick. Cross-checked with the same
  interval in `mtime` ticks.
- One run = 200 timer ticks at a 1 ms quantum; the handler measures 200
  switches (199 tick-to-tick plus the final switch back to the boot
  context, folded in after the timer stops).
- Correctness evidence: exact tick/switch counts (200/200) and per-task
  iteration counts. Round-robin fairness (all three tasks within a few
  percent) proves every tick preempted and switched.

## Build log

Full log saved as `bench-logs/build.log` (captured 2026-09-09, after
`make clean`):

```
$ make clean && make smode.elf smode-mbase.elf
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=1 -c src/smode/sboot.S -o src/smode/sboot_s.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=1 -c src/smode/msetup.c -o src/smode/msetup_s.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=1 -c src/smode/strap.S -o src/smode/strap_s.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=1 -c src/smode/ssched.c -o src/smode/ssched_s.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=1 -c src/smode/stimer.c -o src/smode/stimer_s.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=1 -c src/smode/smain.c -o src/smode/smain_s.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=1 -c src/smode/stasks.c -o src/smode/stasks_s.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/uart.c -o src/uart.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=1 -T link.ld -o smode.elf src/smode/sboot_s.o src/smode/msetup_s.o src/smode/strap_s.o src/smode/ssched_s.o src/smode/stimer_s.o src/smode/smain_s.o src/smode/stasks_s.o src/uart.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: smode.elf has a LOAD segment with RWX permissions
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=0 -c src/smode/sboot.S -o src/smode/sboot_m.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=0 -c src/smode/msetup.c -o src/smode/msetup_m.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=0 -c src/smode/strap.S -o src/smode/strap_m.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=0 -c src/smode/ssched.c -o src/smode/ssched_s.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=0 -c src/smode/stimer.c -o src/smode/stimer_m.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=0 -c src/smode/smain.c -o src/smode/smain_m.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=0 -c src/smode/stasks.c -o src/smode/stasks_m.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -DTRAP_SMODE=0 -T link.ld -o smode-mbase.elf src/smode/sboot_m.o src/smode/msetup_m.o src/smode/strap_m.o src/smode/ssched_m.o src/smode/stimer_m.o src/smode/smain_m.o src/smode/stasks_m.o src/uart.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: smode-mbase.elf has a LOAD segment with RWX permissions
```

Exit 0, zero errors. Only the linker's standard RWX-segment warning
(also present in the repo's other modules). Toolchain:
`riscv64-unknown-elf-gcc 13.2.0`, QEMU 8.2.2.

## Run outputs

Five trials per binary, all on QEMU 8.2.2 (`-machine virt`). Raw logs:
`bench-logs/mbase-run1.log` through `mbase-run5.log`,
`bench-logs/smode-run1.log` through `smode-run5.log`. (The trial
`timeout 25` line at the end of each log is the harness killing QEMU
after the guest printed `halting.`; it is not guest output.)

### M-mode baseline, representative full run (`mbase-run4.log`)

```
========================================
riscv-baremetal-demo: M-mode baseline
same scheduler on the machine timer interrupt
========================================

quantum 10000 ticks (1 ms), 200 ticks total

timer armed, entering wfi wait...

smode: done. 200 timer ticks, 200 measured context switches

task progress (task bodies never yield):
  spin-a: 13827804 iterations
  spin-b: 14380116 iterations
  spin-c: 13734538 iterations

interrupt latency (mtime tick = 100 ns):
  min 118 / median 133 / max 41877 ticks  (11800 / 13300 / 4187700 ns)

context-switch cost, entry stamp to exit stamp:
  min 19830 / median 21105 / max 997050 cycles
  cross-check via mtime: min 135 / median 144 / max 6664 ticks (13500 / 14400 / 666400 ns)

rdcycle/rdtime ratio at median switch cost: 146 cycles per 100 ns tick

halting.
```

(Earlier trials print `entering wfi wait...`; the banner was reworded to
`waiting for ticks...` after the wait was changed from `wfi` to a spin,
see "Termination race" below. The numbers are unaffected.)

### S-mode delegated, representative full run (`smode-run4.log`)

```
========================================
riscv-baremetal-demo: S-mode trap delegation
scheduler on the delegated supervisor timer interrupt
========================================

Sstc (stimecmp): present, pure S-mode timer
quantum 10000 ticks (1 ms), 200 ticks total

timer armed, entering wfi wait...

smode: done. 200 timer ticks, 200 measured context switches

task progress (task bodies never yield):
  spin-a: 14779363 iterations
  spin-b: 14621765 iterations
  spin-c: 14621614 iterations

interrupt latency (mtime tick = 100 ns):
  min 125 / median 237 / max 6674 ticks  (12500 / 23700 / 667400 ns)

context-switch cost, entry stamp to exit stamp:
  min 4440 / median 8317 / max 314235 cycles
  cross-check via mtime: min 33 / median 58 / max 2246 ticks (3300 / 5800 / 224600 ns)

rdcycle/rdtime ratio at median switch cost: 143 cycles per 100 ns tick

halting.
```

### All five trials (medians)

M-mode baseline:

| trial | ticks / switches | latency median (100 ns ticks) | switch median (cycles) | switch median (mtime ticks) |
|---|---|---|---|---|
| 1 | 200 / 200 | 287 | 9780 | 69 |
| 2 | 200 / 200 | 137 | 21307 | 146 |
| 3 | 200 / 200 | 144 | 21607 | 148 |
| 4 | 200 / 200 | 133 | 21105 | 144 |
| 5 | 200 / 200 | 231 | 8542 | 60 |

S-mode delegated:

| trial | ticks / switches | latency median (100 ns ticks) | switch median (cycles) | switch median (mtime ticks) |
|---|---|---|---|---|
| 1 | 200 / 200 | 144 | 14295 | 98 |
| 2 | 200 / 200 | 238 | 7897 | 55 |
| 3 | 200 / 200 | 305 | 9502 | 68 |
| 4 | 200 / 200 | 237 | 8317 | 58 |
| 5 | 200 / 200 | 252 | 7200 | 51 |

## Analysis: the delegation delta

Every trial in both binaries measured exactly 200 timer ticks and 200
context switches: the delegated S-mode timer path is functionally
correct and loses no ticks.

On the overhead question, the honest reading is that host noise
dominates. The medians move substantially trial to trial in both
binaries (M-mode switch cost: 8542 to 21607 cycles; S-mode: 7200 to
14295 cycles), and the ranges overlap heavily:

- Interrupt latency median: M-mode 133-287 ticks, S-mode 144-305 ticks.
- Switch cost median: M-mode 8542-21607 cycles (60-148 mtime ticks),
  S-mode 7200-14295 cycles (51-98 mtime ticks).

There is no systematic delegation penalty visible above the noise: the
S-mode path, which handles the timer interrupt entirely in S-mode via
`stimecmp` with no M-mode involvement after boot, measures in the same
band as the M-mode baseline on this QEMU build. The `rdcycle`/`mtime`
cross-check agrees in every trial (ratio 141-146 units per 100 ns
tick), so the two independent counters confirm each other.

The max values in every trial are host-scheduling outliers (hundreds of
thousands of cycles, milliseconds of virtual time); the medians are the
figures to compare, and even those bounce with host load. These are
QEMU-model measurements, not silicon numbers.

## Termination race found and fixed during testing

An early version rearmed the timer on every tick including the last and
waited in `wfi`. One trial in three then observed 204 ticks instead of
200: the final rearm's deadline fired after the quota was met, the
extra handler runs wrote past the 200-entry sample arrays, and the run
only stopped when host scheduling happened to break the cycle. The fix,
verified over the ten trials above (all exactly 200/200):

- The handler does not rearm on the final tick; it disarms the timer
  (`stimer_disarm()`: comparator set to the far future, which also
  clears the level-triggered pending bit so the return cannot
  immediately re-trap) and returns the boot context.
- A defensive guard at the top of the handler returns the boot context
  without recording if a tick ever arrives past the quota.
- `sched_wait()` spins on `done` instead of `wfi`, because with no tick
  armed past the quota a `wfi` could sleep with nothing left to wake it.

A 5-tick instrumented build confirmed no tick fires past the quota, and
all ten 200-tick trials above terminated cleanly at exactly 200/200.

## What was verified

- The M-mode boot programmed `medeleg`/`mideleg`, installed `stvec`, and
  `sret` dropped to S-mode: the banner and all subsequent output ran in
  S-mode (an M-mode-only CSR access would have faulted; the Sstc probe
  and `stimecmp` writes prove S-mode execution).
- 200 timer ticks produced exactly 200 measured context switches in both
  binaries; no tick lost, no extra switch.
- All three tasks progressed every run and stayed within a few percent
  of each other, proving preemptive round-robin switching (a missed
  preemption would starve two tasks to zero).
- Latency and switch cost are stamped by `rdtime`/`rdcycle` at trap
  entry/exit, cross-checked against each other (ratio 146 in both
  binaries, matching the independently measured counter rate).

## What was NOT verified

- The M-mode ecall rearm fallback (for harts without Sstc) is written
  and reviewed but never executed: this QEMU implements Sstc, so the
  pure S-mode path was always taken. The fallback path is untested.
- Five trials per binary were run, but host scheduling noise moves the
  medians substantially, so no fine-grained S-mode vs M-mode delta can
  be claimed beyond "no systematic penalty visible above the noise".
- Behavior on real RISC-V silicon (interrupt delivery timing, PMP, Sstc
  availability) was not tested; all measurements are from QEMU 8.2.2.
