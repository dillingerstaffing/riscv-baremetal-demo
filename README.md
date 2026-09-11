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
  switches tasks, so three tasks that never yield still interleave
  (see `src/preempt/`).
- **S-mode trap delegation** (`smode.elf`, see `src/smode/`): the M-mode
  boot stub delegates supervisor interrupts and S-mode exceptions via
  `medeleg`/`mideleg`, installs `stvec`, opens S-mode memory with a PMP
  NAPOT entry, enables counters with `mcounteren`, probes
  `menvcfg.STCE` for Sstc, then `sret` drops to S-mode. The scheduler
  runs entirely in S-mode on the delegated supervisor timer interrupt
  (`stimecmp`), with no M-mode involvement after boot. A matching
  M-mode baseline shares the scheduler core via `-DTRAP_SMODE`
  (see `src/smode/`).
- **virtio-blk block driver** (`virtio-blk.elf`, see `src/virtio-blk/`):
  discovers the virtio-mmio block device on the bus, runs the spec's
  device status sequence, negotiates the interface (1.x via
  `VIRTIO_F_VERSION_1` when offered, legacy `QueuePFN`/`QueueAlign`
  otherwise), lays out a 128-entry split virtqueue in RAM, and issues a
  real sector write followed by a sector read
  (see `src/virtio-blk/`).
- **SMP bring-up** (`smp.elf`, see `src/smp/`): two-hart startup on
  `-smp 2`. Every hart reads `mhartid`, installs a private stack from a
  static array, and hart 1 spins on a release flag until hart 0 starts
  it. UART output from both harts is serialized with an `amoswap`
  spinlock (see `src/smp/`).
- **Cycle-accurate UART baud check** (`uart-baud.elf`, see
  `src/uart-baud/`): programs the ns16550a divisor latch for divisors
  1, 12, and 96, and measures the bit timing the UART model actually
  produces, using the receive FIFO's character timeout in internal
  loopback, stamped with `rdcycle` calibrated against the 10 MHz
  `mtime` (see `src/uart-baud/`).
- **WFI wakeup latency** (`wfi-latency.elf`, see `src/wfi-latency/`):
  arms the CLINT timer 20000 mtime ticks (2 ms) ahead, then either
  executes `wfi` or spins on a flag, and measures the latency from the
  programmed wakeup to the first instruction after `mret`, with 400
  interleaved trials of each kind per run (first 8 discarded)
  (see `src/wfi-latency/`).
- **PLIC claim/complete** (`plic.elf`, see `src/plic/`): drives the
  platform-level interrupt controller directly through its
  memory-mapped registers. Enables the UART interrupt (source 10) via
  priority/enable/threshold, asserts it with a looped-back UART byte,
  claims it, and completes it (see `src/plic/`).
- **Sv39 page-table walk** (`sv39.elf`, see `src/sv39/`): builds a
  minimal Sv39 table by hand in RAM, enables it with `satp` (MODE=8,
  ASID=0) plus `sfence.vma`, drops from M-mode to S-mode, and exercises
  the hardware walker: a store/load round trip through VA `0x40000000`
  (three-level walk `root[1] -> l1[0] -> l0[0]`, leaf PTE with VRWAD
  flags set), then unmapped loads that die at each walk level
  (see `src/sv39/`).
- **sstatus.SIE interrupt gate** (see `src/sstatus-sie-gate/`): a pending
  supervisor timer interrupt stays pending without trapping while SIE=0,
  and traps exactly once the moment SIE=1.
- **mcause interrupt bit** (see `src/mcause-interrupt-bit/`): mcause bit 63
  distinguishes interrupt from exception, verified via M-mode ecall
  (bit clear) and CLINT timer interrupt (bit set).

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
- lab 10: Preemptive scheduler on the CLINT machine timer with full-context switches (see `src/preempt/`)
- lab 11: Bare-metal UART shell with help/echo/regs/uptime commands (see `src/shell/`)
- lab 12: Scheduler running in S-mode on the delegated supervisor timer interrupt (see `src/smode/`)
- lab 13: PMP no-access denial test with a locked NAPOT region over scratch RAM (see `src/pmp/`)
- lab 14: Misaligned load/store experiment with exact instruction layout (see `src/misaligned/`)
- lab 15: PLIC claim/complete round-trip driving the UART interrupt (see `src/plic/`)
- lab 16: mtimecmp delivery-offset measurement over timed trials (see `src/mtimecmp/`)
- lab 18: CSR readback of misa/marchid/mimpid plus hand-written extension probes (see `src/csr/`)
- lab 19: Misaligned LR/SC experiment (see `src/amo/`)
- ecall: Ecall ABI round-trip, S-mode payload with an M-mode handler that preserves every register (see `src/ecall/`)
- counter-alias: mcycle vs rdcycle lockstep check cross-checked against mtime (see `src/counter-alias/`)
- umode: M-mode to U-mode trap transition via mret with MPP=0 (see `src/umode/`)
- msip: CLINT msip software-interrupt delivery with set/clear cycles (see `src/msip/`)
- mtvec-vectored: Vectored mtvec dispatch of U-mode ecalls and the timer interrupt (see `src/mtvec-vectored/`)
- cycmon: rdcycle monotonicity around a fixed nop window (see `src/cycmon/`)
- fs-check: mstatus.FS field write/readback and SD-bit behavior (see `src/fs-check/`)
- medeleg-mask: medeleg/mideleg writable-mask discovery with write/readback (see `src/medeleg-mask/`)
- mideleg-route: mideleg set to delegate only the supervisor external interrupt (bit 9 readback-verified against the hypervisor-forced bits); pended supervisor timer traps to M-mode (mcause 0x8000000000000005) while the PLIC supervisor-context UART interrupt traps to S-mode (scause 0x8000000000000009, sepc at the interrupted instruction, claim 10), 3 runs PASS (see `src/mideleg-route/`)
- wfi-resume-pc: WFI resume-PC probe, trap entry at wfi and resume at wfi+4 (see `src/wfi-resume-pc/`)
- lrsc-histogram: Aligned LR/SC attempts-to-success histogram on a single hart (see `src/lrsc-histogram/`)
- pmp-tor: PMP TOR boundary test with two entries forming one exact boundary (see `src/pmp-tor/`)
- mpp-encoding: mstatus.MPP encoding write/return/verify across privilege modes (see `src/mpp-encoding/`)
- mcycle-write: mcycle write/readback/advance coherence (see `src/mcycle-write/`)
- mip-msip: CLINT msip pending-bit tracking in mip with interrupts disabled (see `src/mip-msip/`)
- mie-msip: mie MSIE bit as an independent interrupt gate (see `src/mie-msip/`)
- sip-ssip: sip SSIP pending bit with delegation-gated writability (see `src/sip-ssip/`)
- mie-global: mstatus.MIE as the global interrupt gate (see `src/mie-global/`)
- sc-fail: LR/SC failure path, sc with no reservation and mismatched addresses (see `src/sc-fail/`)
- mret-no-restore: Trap handler that clobbers a0-a7 without restoring them (see `src/mret-no-restore/`)
- stvec-direct: S-mode direct-mode stvec setup and trap delivery (see `src/stvec-direct/`)
- mepc-resume-skip: Trap-resume skip by advancing mepc past the faulting instruction (see `src/mepc-resume-skip/`)
- pmp-napot-size: PMP NAPOT size-decoding test over nested regions (see `src/pmp-napot-size/`)
- mtime-write: CLINT mtime write/readback/advance coherence (see `src/mtime-write/`)
- satp-asid: satp ASID write/readback and WARL discovery in S-mode (see `src/satp-asid/`)
- satp-bare: satp MODE=Bare write/readback with nonzero PPN in S-mode, plus the physical-access check that Bare means no address translation (see `src/satp-bare/`)
- satp-mode-warl: satp MODE WARL probe of Bare, Sv39, Sv48, Sv57 and a reserved encoding in M-mode, with an Sv39 through-translation proof in S-mode (see `src/satp-mode-warl/`)
- sstatus-spp-sret-u: sret with SPP=0 drops to U-mode, verified by the illegal-instruction trap landing in M-mode (see `src/sstatus-spp-sret-u/`)
- mtval-fault-address: Load vs store fault mtval agreement at the same address (see `src/mtval-fault-address/`)
- mcounteren: mcounteren, showing M-mode rdcycle is not gated by the enable bits (see `src/mcounteren/`)
- cycle-read-latency: rdcycle read-latency floor from back-to-back csrr reads (see `src/cycle-read-latency/`)
- mtimecmp-oneshot: One-shot mtimecmp disarm from inside the trap handler (see `src/mtimecmp-oneshot/`)
- mie-stie: STIE vs MTIE enable-bit separation (see `src/mie-stie/`)
- mideleg-warl: mideleg WARL probe that publishes the legalized readback of all-ones and zero writes, showing which interrupt causes the hart lets M-mode delegate (see `src/mideleg-warl/`)
- pmp-lock-bit: PMP lock-bit persistence test; a locked NAPOT no-access entry ignores pmpcfg0/pmpaddr0 rewrite attempts and denies M-mode loads with mcause=5 (see `src/pmp-lock-bit/`)
- mcause-warl: mcause WARL software-write probe; backlog premise not reproduced, QEMU 8.2.2 virt implements all 64 bits software-writable (all-ones and zero writes read back, trap entry overwrites regardless), 3 runs byte-identical PASS (see `src/mcause-warl/`)
- stvec-vectored: S-mode vectored stvec, supervisor timer interrupt (code 5) lands at BASE+20 and supervisor external interrupt (code 9, PLIC claim 10) lands at BASE+36, scause 0x8000000000000005 and 0x8000000000000009 respectively, 2 traps, 0 unexpected, mideleg 0x220, 3 runs byte-identical PASS (see `src/stvec-vectored/`)
- sepc-warl: sepc software-write probe; all 64 bits software-writable on QEMU 8.2.2 virt (all-ones write read back 0xffffffffffffffff, zero write read back 0x0), M-mode ecall (cause 11) leaves sepc at the software-written 0 while mepc carries the trap pc (0x800003bc, resumed at +4), 3 runs byte-identical PASS, 14 checks (see `src/sepc-warl/`)
- scause-bit: scause INTERRUPT-bit probe; supervisor timer interrupt reports scause 0x8000000000000005 (bit 63 set, cause 5) and supervisor load page fault reports scause 0x000000000000000d (bit 63 clear, cause 13), 18 checks, 0 mismatches, 3 QEMU 8.2.2 runs PASS (see `src/scause-bit/`)
- stimecmp-one-shot: S-mode stimecmp one-shot disarm, single supervisor timer interrupt then a quiet window with no re-delivery (see `src/stimecmp-one-shot/`)
- mtvec-mode0-direct: mtvec MODE=0 direct trap entry, M-mode ecall and machine timer interrupt both land at BASE (see `src/mtvec-mode0-direct/`)
- sstatus-spp: sstatus.SPP record on delegated S-mode ecall traps; supervisor ecalls delegated via medeleg bit 9, each trap enters the S-mode handler with SPP recording S-mode and scause reporting the S-mode environment call (see `src/sstatus-spp/`)
- sstatus-spp-sret-u: sret with sstatus.SPP=0 drops to U-mode; the U-mode landing pad's privileged sstatus read traps to M-mode with mcause 2, mepc at the read site, and the trapped mstatus.MPP reading U-mode (see `src/sstatus-spp-sret-u/`)
- mie-toggle: mstatus.MIE global interrupt-enable gate in M-mode, 0 traps while MIE clear and exactly 1 trap after MIE set (see `src/mie-toggle/`)
- mscratch-csrrw: csrrw atomic swap on mscratch, sentinel exchange with full restoration (see `src/mscratch-csrrw/`)
- scounteren-trap: mcounteren gating of S-mode rdcycle, illegal-instruction trap when gated and successful read when enabled (see `src/scounteren-trap/`)
- lrsc-store-invalidate: LR/SC reservation invalidation, intervening store makes sc.w fail while the uninterrupted pair succeeds (see `src/lrsc-store-invalidate/`)
- medeleg-ecall: medeleg bit 9 delegation of supervisor environment calls, S-mode ecall lands in the S-mode handler when delegated and traps to M-mode when cleared (see `src/medeleg-ecall/`)
- sip-write-probe: sip WARL write/readback probe; only the SSIP bit is software-writable and only while mideleg delegates SSI, with mip tracking the pending bits as the read-only alias (see `src/sip-write-probe/`)
- mip-pending-no-trap: mip MSIP pending bit tracked with the MIE gate off, set on msip write and clear on release, 0 traps over a 100,000-cycle window (see `src/mip-pending-no-trap/`)
- mcounteren-time-gate: mcounteren TM bit gates S-mode rdtime independently of the cycle counter; with mcounteren=0x1 (CY set, TM clear), S-mode rdtime raises an illegal-instruction trap (scause=0x2, sepc at the rdtime site) while rdcycle reads and advances, 11 checks, 0 mismatches, 3 QEMU 8.2.2 runs PASS (see `src/mcounteren-time-gate/`)
- mcounteren-cy-gate: mcounteren CY bit gates S-mode rdcycle; with mcounteren=0x0 an S-mode rdcycle raises an illegal-instruction trap (scause=0x2, sepc exactly at the rdcycle site, destination register untouched) while with mcounteren=0x1 the same read succeeds and samples strictly advance, 15 checks, 0 mismatches, FNV-1a 0x4d74f3e95211308e, 3 QEMU 8.2.2 runs PASS (see `src/mcounteren-cy-gate/`)
- **mstatus.TW trap** (see `src/mstatus-tw-trap/`): setting mstatus.TW makes
  an S-mode `wfi` trap as an illegal instruction, with sepc at the wfi site.
- **mcounteren.IR gate** (see `src/mcounteren-ir-gate/`): mcounteren.IR gates
  S-mode `rdinstret`; a gated read traps illegal-instruction while an
  enabled read returns the retired-instruction count.
- **scause WARL** (see `src/scause-warl/`): scause is WARL in S-mode;
  all-ones and zero writes read back, and a trap overwrites it with
  the real cause.
- **sip.STIP write** (see `src/sip-stip-write/`): S-mode writes to
  sip.STIP are legalized away on QEMU; the readback stays zero.
- **sstatus.SUM gate** (see `src/sstatus-sum/`): sstatus.SUM gates S-mode
  access to U-pages; SUM=0 faults the load, SUM=1 reads the canary.
- **menvcfg.STCE advertisement** (see `src/menvcfg-stce/`): menvcfg.STCE
  writability probe matched against real Sstc presence; S-mode stimecmp
  access works and an armed stimecmp delivers exactly one supervisor
  timer interrupt.
- **scounteren.TM gate** (see `src/scounteren-tm-gate/`): scounteren.TM
  gates U-mode rdtime; a gated read traps as illegal instruction with
  sepc at the rdtime site, an enabled read returns the advancing timer.
- **sie.STIE gate** (see `src/sie-stie-gate/`): sie.STIE is the S-mode
  per-interrupt enable for the supervisor timer interrupt; STIP stays
  pending without trapping while STIE is clear, and traps once STIE opens.
- **scounteren.CY gate** (see `src/scounteren-cy-gate/`): scounteren.CY
  gates U-mode rdcycle; CY clear traps with an illegal-instruction
  exception, CY set returns an advancing count.
- **sstatus.MXR gate** (see `src/sstatus-mxr/`): sstatus.MXR makes
  execute-only pages readable to S-mode loads; MXR=0 faults, MXR=1
  returns the canary.
- **amoadd.w read-modify-write** (see `src/amo-add-atomicity/`): single-hart
  amoadd.w loop checks every returned old value, the exact final word,
  and zero traps; single-hart contract only, no contention claim.
- **sip.SEIP read-only** (see `src/sip-seip-write/`): S-mode all-ones
  and zero writes to sip leave SEIP unchanged while SSIP sticks and
  clears, with a counting M-mode handler proving no trap was involved.
- **mideleg SSIP routing** (see `src/mideleg-ssip-route/`): mideleg
  bit 1 routes the supervisor software interrupt to S-mode; a pended
  SSIP traps exactly once with scause 0x8000000000000001 and 0 M-mode
  traps, while a pending CLINT msip takes no trap of either kind;
  17 checks, 0 mismatches, 3 QEMU 8.2.2 runs byte-identical PASS.
- **scounteren.IR gate** (see `src/scounteren-ir-gate/`): scounteren.IR
  gates U-mode rdinstret; IR clear traps with scause 0x2 and sepc
  exactly at the rdinstret site, IR set returns strictly increasing
  samples; 18 checks, 0 mismatches, FNV-1a 0x9756843e0befd207
  identical on 3 QEMU 8.2.2 runs, Verdict PASS.
- mcountinhibit-cy-gate: M-mode module verifying mcountinhibit.CY freezes mcycle (zero advance over 1000 reads) and resumes it on clear.
- **sie.STIE write/readback** (see `src/sie-stie-write/`): M-mode WARL
  write/readback probe of the sie enable bits; with STIE delegated, an
  all-ones write admits only STIE, and csrs/csrc round-trips bit 5.
- **mcountinhibit.IR freeze/resume** (see `src/mcountinhibit-ir-freeze/`):
  M-mode probe of the mcountinhibit.IR gate; with IR set the
  back-to-back minstret readbacks are identical, and clearing IR
  resumes the counter.
- **menvcfg.STCE write/readback** (see `src/menvcfg-stce-write/`):
  M-mode probe of the menvcfg.STCE control bit; the bit reads back
  exactly as written, and an all-ones WARL probe reports the
  legalized readback.
- **mie.MSIE gate** (see `src/mie-msie-gate/`): mie.MSIE is the M-mode
  enable bit for the machine software interrupt; with MSIE clear a
  pended CLINT msip stays pending with no trap, with MSIE set it
  delivers exactly one M-mode trap.
- **mstatus.MPRV readback** (see `src/mstatus-mprv-readback/`):
  mstatus.MPRV (bit 17) reads back exactly as written; set via csrs
  yields boot|MPRV, clear via csrc restores the boot value, with zero
  traps.
- **mie.MTIE gate** (see `src/mie-mtie-gate/`): mie.MTIE is the M-mode
  enable bit for the machine timer interrupt; with MTIE clear a pended
  timer interrupt stays pending with no trap, with MTIE set it delivers
  exactly one M-mode trap.
- **mideleg MTIP routing** (see `src/mideleg-mtip-route/`): mideleg
  bit 7 will not take on this hart, so an armed machine timer still
  traps in M-mode while the hart sits in S-mode, with zero S-mode
  arrivals.
- **mideleg SEIP routing** (see `src/mideleg-seip-route/`): mideleg
  bit 9 takes on this hart, so a pended supervisor external interrupt
  traps exactly once in S-mode with scause 0x8000000000000009 and
  zero M-mode traps.
- **sip.STIP vs mideleg bit 5** (see `src/sip-stip-mideleg-reconcile/`):
  S-mode all-ones sip write probed with mideleg bit 5 clear and set;
  the readbacks in the two configurations settle which delegation
  setting reproduces the STIP writability premise.
- **medeleg illegal-instruction route** (see `src/medeleg-illegal-inst-route/`):
  medeleg bit 2 selects the trap destination for illegal instructions;
  with the bit clear the trap lands in M-mode, with the bit set it lands
  in S-mode.
- **medeleg breakpoint route** (see `src/medeleg-breakpoint/`):
  medeleg bit 3 selects the trap destination for breakpoint traps;
  with the bit clear an S-mode ebreak lands in M-mode (mcause 0x3),
  with the bit set it lands in S-mode (scause 0x3); 14 checks,
  0 mismatches, FNV-1a 0x934b460eeaf296e0 identical on 3 QEMU 8.2.2
  runs, Verdict PASS.
- **stval illegal-instruction capture** (see `src/stval-illegal-capture/`):
  on an S-mode illegal-instruction trap the hart records the faulting
  encoding in stval.
- **PMP NAPOT encoding write/readback** (see `src/pmp-napot-encode/`):
  NAPOT patterns for 4 KiB, 64 KiB, and 1 MiB regions read back exactly
  from pmpaddr0, and the pmpcfg0 A field takes the NAPOT value.
- **mstatus.FS Off-to-Dirty on an FP write** (see `src/sstatus-fs-dirty/`):
  with FS set to Initial, one fmv.d.x moves mstatus.FS to Dirty with SD
  set, a second FP write keeps it sticky, and clearing restores the boot
  mstatus word exactly, with no trap firing anywhere in the sequence.
## Modules (Index of shipped labs)
- **src/misa-mxl-warl/**: M-mode misa.MXL WARL write probe, verified on QEMU 8.2.2. [src/misa-mxl-warl/](src/misa-mxl-warl/)
- **src/mtvec-direct-vectoring/**: M-mode Direct mtvec vectoring check (illegal-instruction + ecall) to BASE, measured on QEMU 8.2.2. [src/mtvec-direct-vectoring/](src/mtvec-direct-vectoring/)
- **src/medeleg-ecall-u-route/**: medeleg bit 8 routes a U-mode ecall to the S-mode handler; the restore ecall returns medeleg to its boot value. [src/medeleg-ecall-u-route/](src/medeleg-ecall-u-route/)
