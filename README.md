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
- **SMP bring-up** (`smp.elf`, see `src/smp/`): two-hart startup on
  `-smp 2`. Every hart reads `mhartid`, installs a private stack from a
  static array, and hart 1 spins on a release flag until hart 0 starts
  it. UART output from both harts is serialized with an `amoswap`
  spinlock. Measured on QEMU: bring-up latency 1,595,175 cycles (run 1)
  and 3,554,055 cycles (run 2), release stamp to hart 1's first stamp;
  both harts reported `mhartid` 0 and 1 with disjoint stack slices
  (see `src/smp/PROOF.md` for the build log, the raw run output, and
  how each number was measured).

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
