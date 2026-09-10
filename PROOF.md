<!-- PROOF-HEADER
Checks: 3
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: freestanding RV64 kernel boots on QEMU virt and round-robin schedules three tasks with real assembly context switches

## What was built

The root demo of this repo (`demo.elf`): a freestanding 64-bit RISC-V
kernel for QEMU's `virt` machine. It boots directly into M-mode,
drives an ns16550a UART over MMIO, and interleaves three tasks with
real assembly context switches. Exactly one mechanism is under test:
the cooperative round-robin scheduler in `src/sched.c` / `src/switch.S`.

- `src/boot.S`: M-mode entry. QEMU loads the ELF at `0x80000000` and
  jumps to `_start`; it sets up the C stack from `link.ld`, clears
  BSS, and calls `main`.
- `src/sched.c` / `src/sched.h`: cooperative round-robin scheduler.
  Each task owns a private 4 KiB stack and a `context_t` holding the
  callee-saved registers. A new task starts with a faked context
  (sp aimed at its private stack, ra aimed at `task_trampoline`), so
  the first switch into it simply "returns" into the task body.
  `sched_run()` dispatches ready tasks in registration order until
  all are done; `sched_yield()` switches back to the scheduler.
- `src/switch.S`: `swtch(old_ctx, new_ctx)` saves ra/sp/s0-s11 into
  `*old_ctx` and restores them from `*new_ctx`. Only callee-saved
  registers are switched, which the calling convention guarantees is
  sufficient across a plain C call.
- `src/tasks.c`: three workloads, 5 iterations each. Heartbeat prints
  a tick counter; fibonacci prints fib(1)..fib(5) from locals that
  must survive every yield; spinner burns cycles between prints.
- `src/main.c`: UART bring-up, task registration, `sched_run()`,
  then a "all tasks finished" line and a `wfi` halt.

Built with `riscv64-unknown-elf-gcc -O2 -ffreestanding -nostdlib
-nostartfiles -march=rv64imac_zicsr -mabi=lp64`, linked at
`0x80000000` via `link.ld`.

## How it was verified

`demo.elf` was rebuilt from source and run under QEMU 8.2.2
(`qemu-system-riscv64 -machine virt -nographic -bios none -kernel
demo.elf`); the full UART transcript was captured and checked by
script. Three checks, zero mismatches:

1. **Completion.** All three tasks printed all 5 iterations (15 task
   lines total) and the run ended with `scheduler: all tasks
   finished. halting.` No task starved, none ran twice.
2. **Round-robin order.** The 15 task lines interleave strictly as
   heartbeat, fibonacci, spinner, repeated 5 times: the dispatch
   order matches registration order on every round.
3. **Private state survived context switches.** Fibonacci printed
   1, 1, 2, 3, 5, so the task's locals `a` and `b` were intact
   across every one of its yields; the spinner cycled its frames
   `| / - \ |`. Had `swtch` corrupted sp or any s-register, the
   values and the interleaving would both have broken.

`RESULT: PASS` (3/3 checks). The run halts in `wfi` after the
finished line; the QEMU process was stopped by timeout once the
transcript was complete.
