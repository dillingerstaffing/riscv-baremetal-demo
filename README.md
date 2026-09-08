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

## Project layout

```
riscv-baremetal-demo/
  Makefile        build and run (CROSS=riscv64-unknown-elf- by default)
  link.ld         places .text/.rodata/.data/.bss at 0x80000000, 16 KiB boot stack
  src/
    boot.S        _start: stack init, BSS clear, call main, halt on return
    switch.S      swtch(): save/restore callee-saved register context
    uart.h/.c     ns16550a UART driver (polled)
    sched.h/.c    task table, task_create, sched_yield, sched_run
    tasks.h/.c    three demo workloads (heartbeat, fibonacci, spinner)
    main.c        UART init, banner, task registration, scheduler start
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

- Preemptive scheduling via the CLINT timer interrupt (`mtime`/`mtimecmp`)
- UART receive with interrupt-driven input and a tiny shell
- `sbrk`-style heap and dynamic task creation
