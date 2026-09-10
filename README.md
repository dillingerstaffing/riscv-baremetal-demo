# riscv-baremetal-demo

A from-scratch bare-metal kernel for 64-bit RISC-V, running on QEMU's
`virt` machine with no operating system underneath it. It brings up the
board, drives the UART directly, and runs three tasks under a tiny
round-robin scheduler with real assembly context switches.

## What this demonstrates

- **Bare-metal bring-up**: custom entry point in RISC-V assembly, BSS
  clearing, stack setup, linker script placing the image at
  `0x80000000` (start of RAM on the `virt` board).
- **Hardware driver from the datasheet**: polled ns16550a UART driver
  (register-level MMIO at `0x10000000`), with `putc`/`puts` plus decimal
  and hex formatting helpers, written with no library dependencies.
- **A real scheduler, not a loop**: cooperative round-robin scheduler.
  Each task owns a private 4 KiB stack. `swtch` (assembly) saves and
  restores the callee-saved registers (`ra`, `sp`, `s0`-`s11`).
  New tasks start via a faked register context whose return address
  points at a trampoline, so the first switch simply "returns" into
  the task body.
- **Clean, freestanding C**: `-ffreestanding -nostdlib -nostartfiles`,
  no libc, no syscalls, warnings enabled (`-Wall -Wextra`).
- **Preemptive scheduling** (`preempt.elf`, see `src/preempt/`): the CLINT
  machine timer (`mtime`/`mtimecmp`) fires every 1 ms; an assembly trap
  handler saves the full register context (x1-x31 plus `mepc`) and
  switches tasks, so three tasks that never yield still interleave.
  Measured on QEMU: interrupt latency min 13.6 us, context-switch cost
  min 17.3 us (see `src/preempt/PROOF.md` for the build log, the raw run
  output, and how each number was measured).
- **S-mode trap delegation** (`smode.elf`, see `src/smode/`): the M-mode
  boot stub delegates supervisor interrupts and S-mode exceptions via
  `medeleg`/`mideleg`, installs `stvec`, opens S-mode memory with a PMP
  NAPOT entry, enables counters with `mcounteren`, probes
  `menvcfg.STCE` for Sstc, then `sret` drops to S-mode. The scheduler
  runs entirely in S-mode on the delegated supervisor timer interrupt
  (`stimecmp`), with no M-mode involvement after boot. A matching
  M-mode baseline shares the scheduler core via `-DTRAP_SMODE`. Measured
  on QEMU 8.2.2, five trials per binary, 200 ticks each: exact 200/200
  tick and switch counts every trial, latency and switch cost in the
  same band as the M-mode baseline with no systematic delegation
  penalty above host noise (see `src/smode/PROOF.md`).
- **virtio-blk block driver** (`virtio-blk.elf`, see `src/virtio-blk/`):
  discovers the virtio-mmio block device on the bus, runs the spec's
  device status sequence, negotiates the interface (1.x via
  `VIRTIO_F_VERSION_1` when offered, legacy `QueuePFN`/`QueueAlign`
  otherwise), lays out a 128-entry split virtqueue in RAM, and issues a
  real sector write followed by a sector read. Measured on QEMU: 512/512
  bytes round-tripped through sector 0 with 0 mismatches, device status
  OK on both requests (see `src/virtio-blk/PROOF.md` for the build log,
  the raw run output, and a host-side check of the backing image).
- **SMP bring-up** (`smp.elf`, see `src/smp/`): two-hart startup on
  `-smp 2`. Every hart reads `mhartid`, installs a private stack from a
  static array, and hart 1 spins on a release flag until hart 0 starts
  it. UART output from both harts is serialized with an `amoswap`
  spinlock. Measured on QEMU: bring-up latency 1,595,175 cycles (run 1)
  and 3,554,055 cycles (run 2), release stamp to hart 1's first stamp;
  both harts reported `mhartid` 0 and 1 with disjoint stack slices
  (see `src/smp/PROOF.md` for the build log, the raw run output, and
  how each number was measured).
- **Cycle-accurate UART baud check** (`uart-baud.elf`, see
  `src/uart-baud/`): programs the ns16550a divisor latch for divisors
  1, 12, and 96, and measures the bit timing the UART model actually
  produces, using the receive FIFO's character timeout in internal
  loopback, stamped with `rdcycle` calibrated against the 10 MHz
  `mtime`. Measured on QEMU: ppm error shrinks with longer timeouts
  (about 0.3% at divisor 96), the signature of additive host latency on
  a correct 1/D baud law, not a baud error (see `src/uart-baud/PROOF.md`
  for the build log, the raw run output, and the limits of
  verification).
- **WFI wakeup latency** (`wfi-latency.elf`, see `src/wfi-latency/`):
  arms the CLINT timer 20000 mtime ticks (2 ms) ahead, then either
  executes `wfi` or spins on a flag, and measures the latency from the
  programmed wakeup to the first instruction after `mret`, with 400
  interleaved trials of each kind per run (first 8 discarded). Measured
  on QEMU: waking a halted vcpu costs about 1.5 to 1.9x the
  timer-interrupt latency of a spinning hart, roughly 130 to 230 ticks
  (13 to 23 us) of extra vcpu-wakeup latency in the quieter runs; the
  trap path itself is identical either way (see
  `src/wfi-latency/PROOF.md` for the build log, three raw QEMU run
  logs, the clock calibration, and the limits of verification).
- **PLIC claim/complete** (`plic.elf`, see `src/plic/`): drives the
  platform-level interrupt controller directly through its
  memory-mapped registers. Enables the UART interrupt (source 10) via
  priority/enable/threshold, asserts it with a looped-back UART byte,
  claims it, and completes it. Measured on QEMU: claim returns id 10,
  claim step about 29655-32310 `rdcycle` units, complete step about
  29730-40230 units (host-time MMIO costs, calibrated per run against
  the 10 MHz `mtime`), post-complete claim returns 0, identical PASS
  on 3 runs (see `src/plic/PROOF.md` for the build log, the raw run
  output, and the QEMU PLIC model behavior this depends on).
- **Sv39 page-table walk** (`sv39.elf`, see `src/sv39/`): builds a
  minimal Sv39 table by hand in RAM, enables it with `satp` (MODE=8,
  ASID=0) plus `sfence.vma`, drops from M-mode to S-mode, and exercises
  the hardware walker: a store/load round trip through VA `0x40000000`
  (three-level walk `root[1] -> l1[0] -> l0[0]`, leaf PTE `0x20000cc7`
  flags VRWAD), then unmapped loads that die at each walk level.
  Measured on QEMU: round trip returns `0xdeadbeefcafebabe` with the
  physical page holding the same value; all three fault tests report
  `mcause=13` (load page fault) with `mtval` equal to the faulting VA
  (`0x40001000`, `0x50000000`, `0xc0000000`), `mepc` exactly the faulting
  `ld`, and `stval=0` (traps taken in M-mode), identical PASS on 3 runs
  (see `src/sv39/PROOF.md` for the build log, the raw run output, and
  the QEMU walker behavior this depends on).

## Project layout

```
riscv-baremetal-demo/
  Makefile        build and run (CROSS=riscv64-unknown-elf- by default;
                  QEMU=qemu-system-riscv64 is overridable)
  link.ld         places .text/.rodata/.data/.bss at 0x80000000, 16 KiB boot stack
  src/
    boot.S        _start: stack init, BSS clear, call main, halt on return
    switch.S      swtch(): save/restore callee-saved register context
    uart.h/.c     ns16550a UART driver (polled)
    sched.h/.c    task table, task_create, sched_yield, sched_run
    tasks.h/.c    three demo workloads (heartbeat, fibonacci, spinner)
    main.c        UART init, banner, task registration, scheduler start
    preempt/      preemptive-scheduler module (built as preempt.elf)
      trap.S      machine-timer trap entry/exit, full context save/restore
      clint.h/.c  CLINT mtime/mtimecmp driver
      psched.h/.c task table, C trap handler, cycle/latency measurements
      ptasks.h/.c three tasks that never yield
      pmain.c     bring-up, wait loop, results report
      PROOF.md    build log, QEMU run output, measurement analysis
    smp/          two-hart SMP bring-up module (built as smp.elf)
      smp_boot.S  per-hart entry: mhartid read, private stacks, release-flag wait
      spinlock.h/.c  amoswap-based spinlock serializing UART output
      smp_print.c locked UART output helpers
      smp_main.c  hart 0: bring-up, release, verification report
      hart1.c     hart 1: entry stamp, hart-id/stack print, done signal
      smp.h       shared definitions
      PROOF.md    build log, QEMU run output, measurement analysis
    virtio-blk/   virtio-blk block driver module (built as virtio-blk.elf)
      virtio.h/.c virtio-mmio transport: discovery, init sequence,
                  feature negotiation, split-virtqueue management
      blk.h/.c    sector read/write via 3-descriptor request chains
      bmain.c     bring-up, sector write/read/verify, results report
      PROOF.md    build log, QEMU run output, measurement analysis
    uart-baud/    cycle-accurate UART baud check (built as uart-baud.elf)
      baud_main.c divisor latch programming, loopback receive-timeout
                  measurement, rdcycle vs mtime clock calibration
      PROOF.md    measurement definition, three QEMU run logs, limits
    pmp/          PMP no-access denial test (built as pmp.elf)
      pmp_main.c  locked NAPOT no-access region, load/store fault tests
                  with mcause/mepc/mtval verification
      pmp_trap.S  minimal M-mode trap entry recording the fault state
      PROOF.md    build log, three QEMU run logs, config and limits
    wfi-latency/  WFI wakeup latency (built as wfi-latency.elf)
      wfi_main.c  CLINT arming, wfi vs spin trial blocks, interleaved
                  trials, statistics, in-program PASS/FAIL checks
      wfi_trap.S  M-mode trap entry stamping rdtime/rdcycle on entry
      wfi.h       trap save-area layout and per-trial record
      PROOF.md    build log, three QEMU run logs, clock calibration,
                  method, results, and limits
    misaligned/   misaligned load/store experiment (built as mal.elf)
      mis_main.c  M-mode trap vector, aligned control, misaligned lw
                  and sw tests with exact instruction layout
      mis_trap.S  minimal M-mode trap entry recording mcause/mepc/mtval
      PROOF.md    build log, three QEMU run logs, results and limits
    amo/          misaligned LR/SC experiment (built as amo.elf)
      amo_main.c  M-mode trap vector, aligned control, misaligned lr.w,
                  misaligned sc.w after an aligned lr, and a misaligned
                  lr/sc pair, all with exact instruction layout
      amo_trap.S  minimal M-mode trap entry recording mcause/mepc/mtval
      PROOF.md    build log, three QEMU run logs, results and limits
    plic/         PLIC claim/complete round-trip (built as plic.elf)
      plic_main.c PLIC programming, UART-loopback interrupt assertion,
                  timed claim/complete, in-program PASS/FAIL checks
      PROOF.md    build log, three QEMU run logs, QEMU PLIC model notes,
                  results and limits
    mtimecmp/     mtimecmp delivery-offset measurement (built as mtimecmp.elf)
      mt_main.c   CLINT arming 5000 ticks ahead, 1000 timed trials,
                  calibration, statistics, in-program PASS/FAIL checks
      mt_trap.S   M-mode trap entry stamping rdtime/rdcycle on entry
      PROOF.md    build log, three QEMU run logs, results and limits
    sv39/         Sv39 page-table walk and fault path (built as sv39.elf)
      sv39_main.c hand-built 3-level tables, satp/sfence.vma, M->S drop,
                  mapped round trip and 3 fault tests with in-program
                  PASS/FAIL checks
      sv39_trap.S M-mode trap entry recording mcause/mepc/mtval/stval
      PROOF.md    build log, three QEMU run logs, PTE layout, results
                  and limits
    umode/        M-mode to U-mode trap transition (built as umode.elf)
      umode_main.c  mtvec handler, one PMP NAPOT entry, MPP=0 drop,
                    mret into a one-instruction U-mode ecall payload
      umode_trap.S  minimal M-mode trap entry recording mcause/mepc/mtval
      PROOF.md    build log, three QEMU run logs, results and limits
    msip/         CLINT msip software-interrupt delivery (built as msip.elf)
      msip_main.c   two set/clear cycles, trap-count and mcause checks,
                    quiet windows verifying no re-delivery after clear
      msip_trap.S   M-mode trap entry recording mcause/mepc/mtval
      PROOF.md    build log, three QEMU run logs, results and limits
```

## How to build and run

Prerequisites (Debian/Ubuntu):

```sh
sudo apt-get install gcc-riscv64-unknown-elf qemu-system-misc
```

Build and run:

```sh
make
make run
```

Build and run the preemptive-scheduler module:

```sh
make preempt.elf
make run-preempt
```

Build and run the SMP bring-up module (two harts):

```sh
make smp.elf
make run-smp
```

Build and run the virtio-blk module (needs the raw disk image, created
by the `disk.img` make target):

```sh
make virtio-blk.elf
make run-virtio
```

`make run` launches:

```sh
qemu-system-riscv64 -machine virt -nographic -bios none -kernel demo.elf
```

(`-bios none` skips firmware so the CPU boots straight into `_start`
in M-mode; `-nographic` wires the virtual UART to your terminal.
QEMU keeps running after the demo halts, so stop it with `Ctrl-A X`.)

Using the xPack toolchain instead of the distro package also works:

```sh
make CROSS=riscv-none-elf-
```

## Sample console output

Verified on `qemu-system-riscv64` 8.2.2, `virt` machine:

```
========================================
riscv-baremetal-demo: QEMU virt (rv64imac)
UART driver + cooperative round-robin scheduler
========================================

scheduler: starting 3 tasks...

[heartbeat] tick 1 (task 1 of 3)
[fibonacci] fib(1) = 1 (task 2 of 3)
[spinner] working | (task 3 of 3)
[heartbeat] tick 2 (task 1 of 3)
[fibonacci] fib(2) = 1 (task 2 of 3)
[spinner] working / (task 3 of 3)
[heartbeat] tick 3 (task 1 of 3)
[fibonacci] fib(3) = 2 (task 2 of 3)
[spinner] working - (task 3 of 3)
[heartbeat] tick 4 (task 1 of 3)
[fibonacci] fib(4) = 3 (task 2 of 3)
[spinner] working \ (task 3 of 3)
[heartbeat] tick 5 (task 1 of 3)
[fibonacci] fib(5) = 5 (task 2 of 3)
[spinner] working | (task 3 of 3)

scheduler: all tasks finished. halting.
```

Notice the interleaving: each task prints one line, yields, and the next
task runs. The Fibonacci task keeps its locals (`a`, `b`) across yields,
which only works because every task runs on its own stack.

## Possible extensions

- UART receive with interrupt-driven input and a tiny shell
- `sbrk`-style heap and dynamic task creation

## Labs
- lab 10: Preempt benchmarks, code size -O2 3831 vs -O0 4761 bytes, switch cost measured and mtime cross-checked (commit 3971bb4)
- lab 11: Bare-metal UART shell, help/echo/regs/uptime, verified on QEMU (commit 06c817b)
- lab 12: S-mode trap delegation, scheduler in S-mode via medeleg/mideleg, exact 200/200 tick and switch counts, no delegation penalty above noise (commit 19ed2a7)
- lab 13: PMP no-access denial test, locked NAPOT region over 4 KiB scratch, load traps mcause=5 mepc=0x80000416, store traps mcause=7 mepc=0x800004be, mtval=0x80002000 both, verified on QEMU
- lab 14: Misaligned load/store experiment, M-mode trap handler recording mcause/mepc/mtval, QEMU 8.2.2 virt completes misaligned lw/sw transparently (no traps): lw loaded 0xffffffffd5a1b2c3 matching the sign-extended byte-wise reference, sw round-tripped 0x12345678 exactly with the neighboring byte untouched, identical across 3 runs
- lab 15: PLIC claim/complete round-trip, source 10 (UART0) enabled via priority/enable/threshold, asserted with a looped-back UART byte: claim returns id 10 (29655-32310 rdcycle units across 3 runs), complete 29730-40230 units (host-time MMIO costs, mtime-calibrated), post-complete claim returns 0, RESULT: PASS on all 3 runs
- lab 16: mtimecmp delivery-offset measurement (mtimecmp.elf), mtimecmp armed 5000 ticks ahead of mtime, 1000 trials per run x 3 runs on QEMU 8.2.2: delivery offset 102-115 min, 152-198 median, 814-881 p99, 992-998 max ticks; rdcycle/mtime calibration 149 on every run; every trial delivered exactly one timer interrupt, zero spurious traps, offset never negative; RESULT: PASS on all 3 runs
- lab 18: CSR alias check (csr.elf), bare-metal M-mode readback of misa/marchid/mimpid plus seven hand-written probe instructions covering the A, C, D, F, H, I, and M letters under a trap handler recording mcause/mepc (the S and U letters are reported by misa but not instruction-probed): misa 0x80000000001411ad (MXL=2 RV64, letters ACDFHIMSU, double-read stable), marchid 0x0, mimpid 0x0, all probes PASS on QEMU 8.2.2 (amoswap.w, c.addi, fadd.d, fadd.s, hfence.gvma, add, mul) with exact expected results and no traps, verified on 3 runs, RESULT: PASS
- ecall ABI round-trip (ecall.elf), bare-metal M-mode to S-mode drop with environment calls: M-mode trap handler saves and restores every register x1-x31 around the C dispatcher, per-call ground truth mcause=9 and the instruction word at mepc=0x00000073 (the ecall encoding), handler-received a0-a5 compared word for word against the payload's loaded constants and returned a0 checked against an independently computed XOR, program output byte-identical across 3 QEMU 8.2.2 runs, RESULT: PASS each
- counter-alias check (counter-alias.elf), bare-metal M-mode mcycle vs rdcycle lockstep read back-to-back over 1000 samples cross-checked against mtime: calibration 119/119 (exact agreement) on all 3 QEMU 8.2.2 runs, back-to-back delta min=108 median=120, counter rate diff 1-2%, no backward counter and no borrow, RESULT: PASS on all 3 runs
- lab 19: Misaligned LR/SC experiment (amo.elf), M-mode trap handler recording mcause/mepc/mtval: misaligned lr.w at base+2 traps every run (mcause=4, mepc exactly the faulting lr, mtval the faulting address), misaligned sc.w after an aligned lr does not trap but returns 1 (reservation dropped, memory unchanged), misaligned lr/sc pair unreachable since the lr traps, identical across 3 runs, RESULT: PASS
- M-mode to U-mode trap transition (umode.elf), M-mode trap handler recording mcause/mepc/mtval, one PMP NAPOT R/W/X entry, mret with mstatus.MPP=0 into a one-instruction U-mode ecall payload: mcause=0x8 (ecall from U-mode; a failed drop would have raised 11), mepc=0x80000440 exactly the payload ecall, mtval=0x0, trap-entry mstatus MPP bits = 0 (U-mode), exactly 1 trap per run, byte-identical across 3 QEMU 8.2.2 runs, RESULT: PASS
- msip software-interrupt delivery (msip.elf), bare-metal M-mode trap handler recording mcause/mepc/mtval: CLINT msip set for hart 0 fires exactly one machine software interrupt (mcause=0x8000000000000003, trap-entry mepc exactly the waiting instruction) over two set/clear cycles, a quiet window after each clear shows zero re-delivery traps, delivery latency 77670/79860/82155 rdcycle units (cycle 1) and 27240/27255/34305 (cycle 2) across 3 QEMU 8.2.2 runs, RESULT: PASS on all 3 runs
- mtvec vectored dispatch (mtvec-vectored.elf), bare-metal RV64 M-mode: mtvec programmed to 0x80000201 (BASE 0x80000200, MODE=1) over a 16-entry table of jal stubs; two U-mode ecalls trap with mcause=0x8 and land at BASE (synchronous exceptions enter at BASE on QEMU 8.2.2, not BASE+32), one machine timer interrupt traps with mcause=0x8000000000000007 and lands at 0x8000021c = BASE+28; mtimecmp disarmed to all-ones after the handler, no refire; RESULT: PASS, output byte-identical across 3 QEMU 8.2.2 runs
- rdcycle monotonicity (cycmon.elf), bare-metal M-mode reads of the `cycle` CSR around a fixed 100-nop window, 1000 samples per run x 3 runs on QEMU 8.2.2 (disassembly-verified: exactly 100 nops between the two reads, nothing else): every delta strictly positive, 0 backward reads, min/median/max deltas 135/150/34620, 135/135/33735, 135/150/48060 host ticks across the 3 runs, 0 outliers above 2^20 ticks each run; PASS shuts down via the virt test-device finisher (QEMU exit 0), FAIL parks the hart (observed timeout exit 124 on a host-stall trip of the earlier absolute bound, which became the 1% outlier allowance); RESULT: PASS on all 3 runs
- mstatus.FS field write/readback (fs-check.elf), bare-metal M-mode writes of the FS field (bits 14:13) through 0, 1, 2, 3 with full-word `mstatus` readback after each write, two iterations per run x 3 runs on QEMU 8.2.2 (boot baseline 0xa00000000): FS read back exactly the written value on all 8 writes every run, every non-FS bit unchanged except SD (bit 63), which reads 1 exactly when FS=3 (the OR-reduction behavior, confirmed in QEMU source); 3 runs byte-identical, RESULT: PASS on all 3 runs

- src/cycmon/: 1000 rdcycle deltas around 100 exact nops, 3 runs, min 135 ticks, median 135-150, max 34-48k ticks, 0 backward reads, PASS

- src/fs-check/: mstatus.FS field written 0..3, 8 write/readback pairs x 3 runs, FS readback == written on every write, only SD moves (FS=3), 3 runs byte-identical, PASS
- src/medeleg-mask/: medeleg/mideleg all-ones write/readback on QEMU 8.2.2, 3 runs: medeleg writable mask 0xf0bfff, mideleg writable mask 0x3666, both restored to boot values (mideleg boot is nonzero 0x1444), pre/post-restore M-mode ecall traps match exactly (mcause=0xb), PASS x3
