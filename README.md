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
    sip-ssip/     sip SSIP pending bit, delegation-gated writability (built as sip-ssip.elf)
      sip_main.c    mideleg-delegation phases, csrs/csrc set/clear cycles,
                    sip/mip readback checks, trap-count check
      sip_trap.S    M-mode trap entry recording mcause/mepc/mtval
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
- src/wfi-resume-pc/: M-mode probe on QEMU 8.2.2, 3 runs x 3 traps: machine software interrupt taken with mepc at wfi (0x80000406), handler advances saved mepc by 4, mret resumes at 0x8000040a = wfi+4, all 31 registers bit-identical pre/post on every trap, mcause 0x8000000000000003, RESULT: PASS x3
- src/lrsc-histogram/: 10000 aligned lr.w/sc.w pairs, single hart, no contention, on QEMU 8.2.2, 3 runs: attempts-to-success histogram is a single spike, attempts=1 count=10000 every run, max attempts 1, all 10000 stored values verified by readback, 0 traps, 3 runs byte-identical, RESULT: PASS x3
- src/pmp-tor/: PMP TOR boundary test (pmp-tor.elf), two TOR entries forming one exact boundary on QEMU 8.2.2, entry 0 allows [0, 0x80002000), entry 1 denies [0x80002000, 0x80003000) locked (pmpcfg0 0x880f, clear attempt leaves 0x8800): lbu of last allowed byte 0x80001fff completes with no trap returning the 0x5a sentinel, lbu of first denied byte 0x80002000 traps mcause=0x5 with mepc=0x80000272 exactly the faulting instruction and mtval=0x80002000 exactly the faulting address, 3 runs byte-identical, RESULT: PASS x3
- src/mpp-encoding/: mstatus.MPP encoding write/return/verify (mpp-encoding.elf), bare-metal M-mode on QEMU 8.2.2: writes the MPP field through all four encodings, reads back the CSR, and mrets into a one-instruction ecall payload at 0x800002d2, recording mcause/mepc/mtval and the trap-entry MPP bits. Measured tuples: 00->(mcause 0x8, MPP 0), 01->(0x9, 1), 11->(0xb, 3); the reserved 10 write is coerced to 00 at write time (readback 0) and behaves as U (mcause 0x8, entry MPP 0). mepc 0x800002d2 exactly the payload ecall and mtval 0x0 on every phase, exactly 1 trap per phase, 3 runs byte-identical, RESULT: PASS x3
- src/mcycle-write/: mcycle write/readback/advance (mcycle-write.elf), bare-metal M-mode on QEMU 8.2.2: csrw writes 0x100000000, immediate readbacks 0x100001e1e/0x100001d5b/0x100001cc5 across 3 runs (delta ~7.4-7.7k host-tick units, write takes effect on this emulator), 4 further readbacks strictly advancing above the written base every run, baseline double-read deltas 6870/6885/6915; not byte-identical across runs (host-clock time base) but structurally identical, RESULT: PASS x3
- src/mip-msip/: CLINT msip pending-bit tracking in mip (mip-msip.elf), bare-metal M-mode on QEMU 8.2.2 with interrupts disabled: writes the CLINT msip register and reads mip to verify the MSIP pending bit (bit 3) sets and clears with the msip write/clear, without enabling the interrupt. Boot mip observed 0x80 (MTIP pending), tracked and conserved across the test; non-MSIP bits unchanged by msip operations. 3 runs, RESULT: PASS x3
- src/mie-msip/: mie MSIE bit as independent interrupt gate (mie-msip.elf), bare-metal M-mode on QEMU 8.2.2: with MSIE clear, CLINT msip=1 produces 0 traps; with MSIE set, msip triggers exactly 1 machine software interrupt (mcause 0x8000000000000003). mie readbacks: 0x0 at boot, 0x0 after clear, 0x8 after set, 0x0 after clear. 3 runs byte-identical, RESULT: PASS x3
- src/sip-ssip/: sip SSIP (bit 1) delegation-gated writability (sip-ssip.elf), bare-metal M-mode on QEMU 8.2.2 with mie.MSIE and mstatus.MIE read back clear throughout: with the supervisor software interrupt not delegated (boot mideleg 0x1444), csrs sip bit 1 is dropped, sip stays 0x0 and mip bit 1 stays clear; after delegating bit 1 in mideleg (readback 0x1446), csrs sip bit 1 sets sip to 0x2 with all other sip bits unchanged and mip bit 1 follows (mip 0x82, the boot 0x80 MTIP conserved), then csrc sip bit 1 returns sip to 0x0 and clears mip bit 1; mideleg restored to 0x1444 with sip back at 0x0; trap count 0 on all phases, 3 runs byte-identical, RESULT: PASS x3
- src/mie-global/: mstatus.MIE global interrupt gate (mie-global.elf), bare-metal M-mode on QEMU 8.2.2 with mie MSIE=1 held constant and msip driven by the 32-bit CLINT access form (64-bit msip accesses fault on this emulator, measured in src/msip/): msip=1 with MIE=0 gives 0 traps and the pending bit stays set in mip; setting MIE fires exactly 1 machine software interrupt (mcause 0x8000000000000003, trap-entry mstatus shows MIE=0/MPIE=1/MPP=3), no re-delivery after msip clear; re-clearing MIE re-arms the gate. mstatus readbacks 0xa00000000 (MIE=0) and 0xa00000008 written (read back 0xa00000088 because the trap+mret interleaving sets MPIE before the readback, documented as a delivery-promptness measurement). 3 runs byte-identical, RESULT: PASS x3
- src/sc-fail/: LR/SC failure path (sc-fail.elf), bare-metal M-mode on QEMU 8.2.2: sc.w with no preceding lr.w returns 1 (no reservation), lr.w on address A followed by sc.w on address B returns 1 (mismatched address), memory unchanged in both cases, zero traps across 3 runs; control test (sc.w matching the lr.w reservation) returns 0. RESULT: PASS x3
- src/mret-no-restore/: M-mode trap handler that clobbers a0..a7 with no restore (mret-no-restore.elf), bare-metal M-mode on QEMU 8.2.2: one ecall traps exactly once (mcause=0xb, mepc=0x80000556 exactly the ecall, mtval=0x0, insn at mepc 0x73), the pre-trap caller loaded a0..a7 with 0x1111...-0x8888... sentinels and reads back the handler's 0x9999...-0x123456789abcdef0 set after mret, all 8 registers match the handler values, proving the caller-saved convention is a software contract rather than hardware. 3 runs byte-identical, RESULT: PASS x3
- src/stvec-direct/: S-mode direct-mode stvec setup and trap delivery (stvec-direct.elf), bare-metal on QEMU 8.2.2: stvec programmed with MODE=00 (direct) reads back 0x800001c8 with mode bits clear, medeleg 0x200, one S-mode ecall traps exactly once (scause 0x9, sepc 0x8000058e exactly the ecall, traps = 1). 3 runs byte-identical, RESULT: PASS x3
- src/mepc-resume-skip/: trap-resume skip (mepc-resume-skip.elf), bare-metal M-mode on QEMU 8.2.2: handler adds 4 to mepc before mret; one 4-byte faulting lw at an unmapped address traps exactly once (mcause=0x5 load access fault, mtval=0xffffffffc0000000, mepc 0x80000312 at trap entry, 0x80000316 after the +4), the instruction at faulting+4 ran (marker 0xdeadbeefdeadbeef) and the faulting load never committed (a0 sentinel 0xa0a0a0a0a0a0a0a0 intact, a1 unchanged); fault/resume addresses taken with in-asm numeric local labels. 3 runs byte-identical, RESULT: PASS x3
- src/pmp-napot-size/: PMP NAPOT size-decoding test (pmp-napot-size.elf), bare-metal M-mode on QEMU 8.2.2: two locked no-access NAPOT entries over one 64 KiB scratch region at base 0x80020000, entry 0 pmpaddr0=0x200081ff (trailing-ones 0x1ff, 4 KiB [0x80020000, 0x80021000)), entry 1 pmpaddr1=0x20009fff (trailing-ones 0x1fff, 64 KiB [0x80020000, 0x80030000)), config bytes 0x98 (L=1, A=NAPOT, no perms), pmpcfg0 reads back 0x9898 proving the locked byte 0 held while byte 1 installed. Phase A (4 KiB only): lbu at 0x80022000 completes with no trap returning pattern byte 0x00, lbu at 0x80020fff (last byte inside) traps mcause=0x5 with mepc=0x80000270 exactly the faulting instruction (resume-8, disassembly-verified) and mtval=0x80020fff exactly the faulting address, lbu at 0x80021000 (first byte outside) completes with no trap. Phase B (64 KiB added): the same lbu at 0x80022000 now traps mcause=0x5 with mepc=0x80000270 and mtval=0x80022000, lbu at 0x8002ffff (last byte inside large region) traps mcause=0x5 mtval=0x8002ffff, lbu at 0x80030000 (first byte outside) completes with no trap, dedicated outside sentinel reads back 0xa5 with no trap. 3 runs byte-identical, RESULT: PASS x3
- src/mtime-write/: CLINT mtime write/readback/advance coherence (mtime-write.elf), bare-metal M-mode on QEMU 8.2.2: wrote 0x100000000 to mtime (0x0200bff8) as two 32-bit stores (low word then high word), read back with a stable-pair 32-bit read (high, low, high with agreement), no 64-bit CLINT access ever issued. Write/readback/delta triples across 3 runs: written 0x100000000 every run, readbacks 0x1000002b2 / 0x10000021d / 0x1000001f9, deltas 690 / 541 / 505 mtime ticks (10 MHz); the written value is unreachable by natural advancement (baseline ~1M ticks at write time), so the readback match genuinely discriminates a stuck write. Post-write advance samples strictly increasing and above the written base on all runs; rdcycle-vs-mtime calibration 149 on all 3 runs. Not byte-identical across runs (host-clock time base) but structurally identical, RESULT: PASS x3
