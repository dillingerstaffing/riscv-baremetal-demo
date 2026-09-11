<!-- PROOF-HEADER
Checks: 19
Mismatches: 0
Checksum: 0x7b32d6aae4e318ef
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF.md: mtvec direct-mode single-entry trap landing (backlog item 138)

## What was built

A bare-metal RV64 module that writes `mtvec` with MODE=0 (direct),
reads it back, then provokes two real traps and checks, from the
address of the handler entry that actually ran, that each trap
landed at the single BASE address, not at BASE + 4*cause.

Files: `d0_trap.S`, `d0_main.c`. Raw UART logs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(byte-for-byte identical across three boots; each run exited QEMU
with status 0 via the virt test-device finisher, which is written
only on PASS).

## Measured results (QEMU 8.2.2, `virt`, all three runs)

From `run1.log` (identical in run2/run3):

```
mtvec-mode0-direct: direct-mode single-entry trap test
mtvec: written=0x800001c8 readback=0x800001c8
control: traps-before-ecall=0 (expect 0)
ecall: count=1 mcause=0xb landing=0x800001c8 (expect 0x800001c8) mepc=0x80000476 (expect 0x80000476)
timer: armed-after-retries=1
timer: waiting...
timer: count=1 mcause=0x8000000000000007 landing=0x800001c8 (expect 0x800001c8)
timer: mtimecmp-after-handler=0xffffffffffffffff (expect 0xffffffffffffffff)
timer: count-after-quiet=1 (expect 1)
checksum: fnv1a64=0x7b32d6aae4e318ef
RESULT: PASS (M-mode ecall->BASE mcause=11, timer->BASE mcause=0x8000000000000007, 0 traps elsewhere)
done
```

Decoded:

- `mtvec` = `0x800001c8`: BASE `0x800001c8`, MODE 0 (direct).
  Readback equals the written value; the mode bits read 0 and the
  base equals the link-time address of the single trap entry
  `d0_trap_entry`.
- M-mode ecall: one trap, `mcause = 0xb` (11), landing
  `0x800001c8` = BASE, recorded `mepc = 0x80000476` = the ecall's
  own address (captured with an in-asm numeric local label).
- Machine timer interrupt: one trap,
  `mcause = 0x8000000000000007`, landing `0x800001c8` = BASE.
- After the timer trap, the handler disarmed the source:
  `mtimecmp` reads `0xffffffffffffffff`, and a further quiet window
  produced no second delivery (`count-after-quiet=1`).
- A control window before any trigger produced zero traps, and the
  unexpected-trap counter stayed 0 throughout: no trap entered with
  any other `mcause`, and every recorded landing equaled BASE, so no
  trap landed anywhere else.
- 19 `check()` assertions, all passing, zero mismatches.

## A correction the measurement forced

The backlog item specified `mcause` 9 for the M-mode ecall. That is
not what the hardware does: an `ecall` executed in M-mode raises
exception code **11** (the M-mode environment call; 8 is the U-mode
ecall, 9 the S-mode ecall). Three independent in-repo ground truths
agree: QEMU 8.2.2's own interrupt log for this run reports
`cause:000000000000000b, desc=machine_ecall`; the sibling
`src/mcause-warl` module measured `mcause = 11` for a deliberate
M-mode ecall twice; and the sibling `src/mtvec-vectored` module's
comments note "an M-mode ecall would raise code 11". The first boot
of this module, built with the spec's 9, fell into an infinite
re-trap loop (the handler classified the trap as unexpected and
resumed at the ecall instead of past it), which is exactly the
behavior an honest `mcause == 11` check eliminates. The module
asserts the measured value 11, and the checksum below commits to it.

## Checksum

`checksum: fnv1a64=0x7b32d6aae4e318ef` is the FNV-1a 64-bit hash
(seed `0xcbf29ce484222325`) computed at run time over, in order: the
`mtvec` written value, the `mtvec` readback, the ecall `mcause`, the
ecall landing address, the ecall `mepc`, the timer `mcause`, and the
timer landing address. It commits to every number that carries the
claim; nothing else feeds it.

## How the landing was observed, not assumed

The trap handler is the only trap entry in the image. On entry it
records the link-time address of its own entry label (resolved
exactly by the assembler's `la`, not by arithmetic on `mtvec`) into
the per-trap record before doing anything else. The main program
then compares that recorded address against the BASE read back from
`mtvec`. A trap that entered anywhere else would record a different
address and fail the check; the unexpected-trap counter would catch
any `mcause` the two deliberate triggers do not explain.

## Build log (genuine)

```
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mtvec-mode0-direct/d0_trap.S -o src/mtvec-mode0-direct/d0_trap.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mtvec-mode0-direct/d0_main.c -o src/mtvec-mode0-direct/d0_main.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o mtvec-mode0-direct.elf src/boot.o src/uart.o src/mtvec-mode0-direct/d0_trap.o src/mtvec-mode0-direct/d0_main.o
ld: warning: mtvec-mode0-direct.elf has a LOAD segment with RWX permissions
```

Built with the xpack riscv-none-elf GCC 15.2.0 toolchain (the
`riscv64-unknown-elf-` prefix the Makefile defaults to is not on
this machine's PATH, so the build used `CROSS=riscv-none-elf-`);
QEMU run with
`LD_LIBRARY_PATH=~/workspace/qemu/usr/lib/x86_64-linux-gnu:~/workspace/qemu/lib/x86_64-linux-gnu`.

## Limits, stated honestly

- These are emulator measurements on QEMU 8.2.2's `virt` machine,
  not silicon. Direct-mode single-entry dispatch is defined by the
  privileged architecture, and the addresses/`mcause` values above
  are what this emulator produces; a real core's timing and reset
  `mtvec` value are not covered.
- The "no trap landed elsewhere" claim rests on the handler being
  the only trap entry in the image plus the landing cross-check; it
  does not cover traps taken before `mtvec` is installed (the
  control window with zero traps bounds that).
- The timer arming uses a bounded retry (up to 10) against the
  known past-deadline hazard; all three runs armed on the first
  attempt (`armed-after-retries=1`).
