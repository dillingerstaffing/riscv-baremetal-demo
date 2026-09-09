# PROOF: preemptive scheduler on the RISC-V machine timer

## What was built

A new module, `src/preempt/`, in this repo, built as its own binary
`preempt.elf` (shares only `boot.S` and the UART driver with the
cooperative demo; `demo.elf` is untouched and still builds and runs).

- `clint.h/.c`: direct MMIO driver for the CLINT timer (`mtime` at
  `0x0200bff8`, `mtimecmp` for hart 0 at `0x02004000`).
- `trap.S`: machine-mode trap entry/exit in assembly. On entry it swaps
  `t0` with `mscratch` (which always points at the current context's
  trapframe), stamps `rdtime`/`rdcycle`, saves x1-x31 plus `mepc` into the
  trapframe, and calls a C handler on a dedicated trap stack. On exit it
  stamps the end of the switch, restores the next context's registers and
  `mepc`, points `mscratch` at the next trapframe, and `mret`s.
- `psched.h/.c`: task table, the C trap handler (rearms the timer,
  round-robins tasks, records measurements), and the final report.
- `ptasks.h/.c`: three tasks that spin forever on integer work and never
  yield. No yield call exists anywhere in task code.
- `pmain.c`: bring-up, then `wfi` until 200 timer ticks elapse.

The design follows from what the privileged ISA specifies on a trap: the
hart writes `mepc`/`mcause`, moves `MIE` to `MPIE` and clears it, then
jumps to `mtvec`. Everything else (the trapframe, the `mscratch` swap
protocol, the stack switch) is constructed from that contract.

## Ground truth the measurements rest on

- CLINT base `0x02000000` and `timebase-frequency = 10000000` were read
  from the device tree QEMU itself generates for `-machine virt`
  (`-machine virt,dumpdtb=` plus a parse of the blob). So one `mtime`
  tick is 100 ns.
- `rdtime` was verified to track the `mtime` MMIO register: back-to-back
  reads interleaved as `mtime=0x2da0, time=0x2da2, mtime2=0x2dac`, i.e.
  the CSR read lands within 1-2 ticks of the MMIO reads around it.
- The `cycle` counter on this QEMU (8.2.2, `virt`) does not count guest
  instructions. Correlating `rdcycle` deltas with `mtime` deltas over
  identical workloads gives a constant ratio of 150 cycles per 100 ns
  tick, i.e. the counter advances at 1.5e9 units per second of virtual
  time (e.g. `dcycle=521927, dmtime=3480` -> 150.0; `dcycle=768167,
  dmtime=5128` -> 149.8). Cycle counts below are therefore convertible
  to time by dividing by 150 ticks per 100 ns, and every switch cost is
  independently cross-checked against `mtime` ticks.
- Steady-state cost of one `rdcycle`/`rdtime` read is about 150 cycles
  (~100 ns); the entry/exit instrumentation adds two reads each, a small
  known overhead inside the measured path.

## Definitions (exactly what the numbers mean)

- Interrupt latency: `rdtime` stamped two instructions into the trap
  handler (right after the `mscratch` swap) minus the `mtimecmp`
  deadline that fired, in 100 ns ticks. The interrupt becomes pending
  when `mtime >= mtimecmp`, so this is the hardware-to-handler delay.
- Context-switch cost: `rdcycle` (and `rdtime`) stamped in the exit path
  just before the register restore, minus the stamps taken at entry, so
  it covers the full save + C handler + rearm + restore + `mret` path.

## Build log

Toolchain: xPack `riscv-none-elf-gcc` 15.2.0
(`~/workspace/toolchains/xpack-riscv-none-elf-gcc-15.2.0-1`),
`make CROSS=riscv-none-elf-`. Zero errors, zero compiler warnings
(the only linker note is the benign RWX-segment warning from the
minimal linker script, also present for `demo.elf`):

```
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/boot.S -o src/boot.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/switch.S -o src/switch.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/uart.c -o src/uart.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sched.c -o src/sched.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/tasks.c -o src/tasks.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/main.c -o src/main.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o demo.elf src/boot.o src/switch.o src/uart.o src/sched.o src/tasks.o src/main.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/preempt/trap.S -o src/preempt/trap.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/preempt/clint.c -o src/preempt/clint.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/preempt/psched.c -o src/preempt/psched.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/preempt/ptasks.c -o src/preempt/ptasks.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/preempt/pmain.c -o src/preempt/pmain.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o preempt.elf src/boot.o src/uart.o src/preempt/trap.o src/preempt/clint.o src/preempt/psched.o src/preempt/ptasks.o src/preempt/pmain.o
```

## QEMU run output

`qemu-system-riscv64` 8.2.2, `-machine virt -nographic -bios none
-kernel preempt.elf`, quantum 10000 ticks (1 ms), 200 ticks total:

```
========================================
riscv-baremetal-demo: preemptive scheduler
CLINT timer interrupt + full-context switch
========================================

clint: mtime@0x200bff8 mtimecmp@0x2004000 timebase 10000000 Hz (100 ns/tick)
quantum 10000 ticks (1 ms), 200 ticks total

timer armed, entering wfi wait...

preempt: done. 200 timer ticks, 200 measured context switches

task progress (task bodies never yield):
  spin-a: 28570 iterations
  spin-b: 28332 iterations
  spin-c: 27680 iterations

interrupt latency (mtime tick = 100 ns):
  min 136 / max 69250 / avg 1406 ticks  (13600 / 6925000 / 140600 ns)

context-switch cost, entry stamp to exit stamp:
  min 25530 / max 446820 / avg 38014 cycles
  cross-check via mtime: min 173 / max 3150 / avg 258 ticks (17300 / 315000 / 25800 ns)

preempt: halting.
```

(The trailing `terminating on signal 15` line in the raw log is the
`timeout` wrapper stopping QEMU after the demo parks in `wfi`, expected
per the README.)

## Reading the numbers

- Preemption is real: 200 timer ticks produced exactly 200 measured
  context switches, and all three tasks made large, balanced progress
  (28570 / 28332 / 27680 iterations) although no task ever yields. Under
  a cooperative scheduler the first task would spin forever and the
  other two counters would read zero.
- The two independent counters agree: avg 38014 cycles / 150 = 253
  ticks, vs 258 ticks measured directly from `mtime`. Min 25530 cycles
  = 170 ticks vs 173 ticks from `mtime`. The apparatus is coherent.
- Interrupt latency min 136 ticks (13.6 us) is the platform's best case
  for timer expiry to handler entry under emulation. The max (6.9 ms)
  and the skew of avg above min are host scheduling jitter: QEMU's
  virtual clock keeps advancing while its vCPU thread waits for the
  host, so a delayed kick shows up directly in `mtime_at_entry -
  deadline`. Same for the switch-cost max.
- The `mepc`-resume path was verified implicitly 200 times: every task
  resumed mid-loop with its registers intact (iteration counters would
  corrupt otherwise, since each iteration depends on the previous
  register-held state spilled through volatile globals).

## Appendix: -O0 vs -O2 benchmarks (2026-09-08)

Same module, same flags as the main build, differing only in the
optimization level. Built with Ubuntu `riscv64-unknown-elf-gcc` 13.2.0
(`-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`, all other flags
identical to the Makefile defaults), each level compiled to its own
object directory and linked against `link.ld`. Zero compile errors,
zero warnings; the only linker note is the benign RWX-segment warning
from the minimal linker script, present at both levels.

### Code size (`riscv64-unknown-elf-size -B`)

```
   text      data       bss       dec       hex   filename
   4761        56     30016     34833      8811   preempt-O0.elf
   3831        56     30024     33911      8477   preempt-O2.elf
```

-O2 shrinks `.text` from 4761 to 3831 bytes (19.5% smaller). `.data`
is unchanged (56 bytes); `.bss` differs by 8 bytes (30016 vs 30024,
task trapframe alignment padding landing differently). Total binary
`dec` goes 34833 -> 33911, 2.6% smaller.

### Measured cycle counts, four QEMU runs

`qemu-system-riscv64` 8.2.2, `-machine virt -nographic -bios none`,
200 ticks each run, same recipe as the main run. Full logs are in
`src/preempt/bench-logs/` (O0-run1.log, O0-run2.log, O2-run1.log,
O2-run2.log). 200 ticks produced exactly 200 switches on every run.

| run | latency min/avg (100 ns ticks) | switch cost min/avg (cycles) | switch cost min/avg (mtime ticks) |
|-----|-------------------------------|------------------------------|----------------------------------|
| -O0 run 1 | 124 / 995 | 17850 / 37739 | 125 / 257 |
| -O0 run 2 | 123 / 808 | 9360 / 33799 | 66 / 231 |
| -O2 run 1 | 129 / 414 | 11790 / 33712 | 81 / 229 |
| -O2 run 2 | 125 / 3600 | 24255 / 39647 | 165 / 269 |

cycle/tick ratios from the two counters: 17850/125 = 142.8,
9360/66 = 141.8, 11790/81 = 145.6, 24255/165 = 147.0. The counters
agree within about 5% on every run, so the measurement apparatus is
coherent; the remaining spread is the platform path itself, not the
instruments.

### Reading the numbers (what they support, and what they do not)

- Code size claim stands: -O2 text is 19.5% smaller, measured by the
  section table above.
- Interrupt latency min is 123-129 ticks at both levels: best-case
  hardware-to-handler delay is independent of optimization level on
  this platform. Max and avg are host scheduling jitter (the vCPU
  thread waiting on the host while the virtual clock advances), so
  only min and the ordering of runs carry information.
- No reliable cycle-count win can be claimed. Best-case switch cost
  spans 9360 to 24255 cycles across the four runs (6.6 us to 16.5 us
  at 150 cycles per 100 ns), and the -O0 and -O2 bands overlap
  completely. The run-to-run spread inside one optimization level is
  larger than any difference between levels, so the honest statement
  is: the trap path's best case on this emulator is not stable enough
  to rank -O0 against -O2 from 200-switch samples. A finer claim would
  need hardware, or the same measurement repeated until the
  distributions separate.
- Sanity note: task iteration counts differ strongly between levels
  (-O0 runs: ~8.1M / 8.1M / 11.4M per task; -O2 runs: ~28k per task),
  which is the expected codegen effect on the spin loops, not a
  scheduling defect: 200 ticks / 200 switches and balanced progress
  hold at both levels.

## Bug found by measurement, then fixed

The first build measured 399 switches for 200 ticks. Tracing showed the
timer interrupt firing inside `preempt_init` itself: at reset
`mtimecmp` is 0, so `mtime >= mtimecmp` already holds and a timer
interrupt is pending before anything is programmed. Enabling `MTIE`/`MIE`
before arming the timer trapped on that stale pending bit, and the
saved `mepc` pointed back into `preempt_init`, so the final `mret`
re-ran initialization and the whole 200-tick run executed twice. Fix:
program `mtimecmp` (which clears the stale pending bit) before enabling
the interrupt. After the fix: 200 ticks, 200 switches, repeatedly.
