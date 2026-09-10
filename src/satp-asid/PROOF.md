<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: satp ASID write/readback + WARL discovery (S-mode)

## What was built

`src/satp-asid/`: a bare-metal RISC-V program that checks the
supervisor address-translation and protection register (`satp`) on
RV64, on the QEMU `virt` board. Two files, sharing only `src/boot.S`
and `src/uart.c` with the other demos. Exactly one mechanism is under
test: the `satp` register's MODE/ASID/PPN fields and their WARL
(write-any-read-legal) behavior.

- `satp_trap.S`: M-mode direct-mode trap entry. No traps are
  expected on the happy path (every `satp` write uses MODE=0, so
  translation is never enabled). If a trap fires it records
  `mcause`/`mepc`/`mtval` into C-visible globals, bumps the trap
  counter, prints the trap record and `RESULT: FAIL`, and parks the
  hart in a `wfi` loop.
- `satp_main.c`: M-mode setup (reads the boot-time `satp`,
  installs the M-mode handler, opens the address space with one PMP
  NAPOT R/W/X entry because S-mode is default-deny) then `mret`s
  with `mstatus.MPP=01` into the S-mode payload `satp_smode_test`,
  which performs the whole experiment in S-mode:
  1. WARL discovery: write `satp` with MODE=0, PPN=0, ASID=all-ones
     (`0xffff << 44`); read back; the writable ASID bits are ground
     truth for the implemented ASIDLEN.
  2. Round-trip: for ASID in {0, 1, mid, max-writable} (exact
     duplicates skipped), `csrw satp` then `csrr` readback; the
     write/readback/extracted-ASID triple is printed and the value
     PASSes only if the readback equals the written value with
     unwritable ASID bits reading back zero, MODE reading back 0,
     and PPN reading back 0.
  3. MODE WARL probe: zero `satp`, then write MODE=15 (a reserved
     encoding) with ASID=0/PPN=0; read back. Translation is never
     enabled. The check pins the observed behavior: the readback
     must equal the pre-write value (the unsupported MODE write
     takes no effect) and the MODE field must be a legal encoding
     (0 through 8; 9-15 are reserved).
  4. Verdict: `traps observed` must be 0 (reaching the completion
     marker proves it, since the handler parks on any trap); prints
     the completion marker and `RESULT: PASS`, then shuts the
     machine down via the virt test-device finisher (QEMU exits 0).
     Any failed check prints `RESULT: FAIL` and parks instead.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make satp-asid.elf` (added to `all` and `clean` in the
Makefile). Run: `qemu-system-riscv64 -machine virt -nographic -bios
none -kernel satp-asid.elf` (`make run-satp-asid`).

## Measured results

Environment: QEMU emulator 8.2.2 (Debian), `-machine virt`,
`-bios none`, `riscv64-unknown-elf-gcc` 13.2.0, `-O2`.

WARL discovery (identical in all 3 runs):

- write `0xffff00000000000` (ASID all-ones, MODE=0, PPN=0),
  readback `0xffff00000000000`
- writable ASID mask = `0xffff`, so discovered **ASIDLEN = 16**
- MODE field readback = 0, PPN field readback = `0x0`
- writable bits form a contiguous low run (checked in code)

Round-trip triples (identical in all 3 runs):

| ASID write          | readback            | extracted ASID | verdict |
|---------------------|---------------------|----------------|---------|
| `0x0`               | `0x0`               | `0x0`          | PASS    |
| `0x100000000000`    | `0x100000000000`    | `0x1`          | PASS    |
| `0x800000000000000` | `0x800000000000000` | `0x8000`       | PASS    |
| `0xffff00000000000` | `0xffff00000000000` | `0xffff`       | PASS    |

MODE WARL probe (identical in all 3 runs):

- write `0xf000000000000000` (MODE=15, reserved), readback `0x0`
- MODE field = 0, ASID field = `0x0`, PPN field = `0x0`
- i.e. the unsupported MODE write took no effect: `satp` kept its
  pre-write value, and the MODE field read back a legal encoding
  (0 = Bare)

Trap counts: 0 in all 3 runs (`traps observed = 0`; the M-mode
handler would have printed a FAIL record and parked on any trap, so
the printed completion marker is the no-trap proof).

Run verdicts: `RESULT: PASS` in all 3 runs; QEMU exit code 0 in all
3 runs. The three raw logs are byte-identical (verified with `diff`).

## Limits

- This is QEMU 8.2.2 on the `virt` machine, not silicon: the
  discovered ASIDLEN=16 and the "unsupported MODE write is ignored"
  behavior are this emulator's implementation choices. Real
  hardware may implement a different ASIDLEN (the discovery
  procedure, not the number, is the portable part).
- The MODE probe deliberately never enables a non-Bare MODE, so the
  interaction of ASID with an active translation regime is outside
  this module's scope (enabling Sv39 with a garbage root PPN would
  fault the next instruction fetch; that experiment needs a valid
  page-table root first).
- Single hart; no interrupts enabled during the run.

## Build log

```
```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/satp-asid/satp_main.c -o src/satp-asid/satp_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o satp-asid.elf src/boot.o src/uart.o src/satp-asid/satp_trap.o src/satp-asid/satp_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: satp-asid.elf has a LOAD segment with RWX permissions
```

## Raw run logs (3 runs, byte-identical)

### run1.log
```

========================================
satp ASID write/readback + WARL discovery
========================================

satp at boot (M-mode read) = 0x0
setup complete; dropping to S-mode...
in S-mode; satp experiment begins

WARL discovery:
  write            = 0xffff00000000000
  readback         = 0xffff00000000000
  writable ASID mask = 0xffff
  discovered ASIDLEN = 16
  MODE field readback = 0
  PPN field readback  = 0x0

round-trip (MODE=0, PPN=0):
  ASID write = 0x0 readback = 0x0 asid = 0x0  PASS
  ASID write = 0x100000000000 readback = 0x100000000000 asid = 0x1  PASS
  ASID write = 0x800000000000000 readback = 0x800000000000000 asid = 0x8000  PASS
  ASID write = 0xffff00000000000 readback = 0xffff00000000000 asid = 0xffff  PASS

MODE WARL probe (write MODE=15, reserved):
  write            = 0xf000000000000000
  readback         = 0x0
  MODE field       = 0
  ASID field       = 0x0
  PPN field        = 0x0

traps observed = 0

COMPLETION MARKER: satp-asid run finished
RESULT: PASS
```

### run2.log
```

========================================
satp ASID write/readback + WARL discovery
========================================

satp at boot (M-mode read) = 0x0
setup complete; dropping to S-mode...
in S-mode; satp experiment begins

WARL discovery:
  write            = 0xffff00000000000
  readback         = 0xffff00000000000
  writable ASID mask = 0xffff
  discovered ASIDLEN = 16
  MODE field readback = 0
  PPN field readback  = 0x0

round-trip (MODE=0, PPN=0):
  ASID write = 0x0 readback = 0x0 asid = 0x0  PASS
  ASID write = 0x100000000000 readback = 0x100000000000 asid = 0x1  PASS
  ASID write = 0x800000000000000 readback = 0x800000000000000 asid = 0x8000  PASS
  ASID write = 0xffff00000000000 readback = 0xffff00000000000 asid = 0xffff  PASS

MODE WARL probe (write MODE=15, reserved):
  write            = 0xf000000000000000
  readback         = 0x0
  MODE field       = 0
  ASID field       = 0x0
  PPN field        = 0x0

traps observed = 0

COMPLETION MARKER: satp-asid run finished
RESULT: PASS
```

### run3.log
```

========================================
satp ASID write/readback + WARL discovery
========================================

satp at boot (M-mode read) = 0x0
setup complete; dropping to S-mode...
in S-mode; satp experiment begins

WARL discovery:
  write            = 0xffff00000000000
  readback         = 0xffff00000000000
  writable ASID mask = 0xffff
  discovered ASIDLEN = 16
  MODE field readback = 0
  PPN field readback  = 0x0

round-trip (MODE=0, PPN=0):
  ASID write = 0x0 readback = 0x0 asid = 0x0  PASS
  ASID write = 0x100000000000 readback = 0x100000000000 asid = 0x1  PASS
  ASID write = 0x800000000000000 readback = 0x800000000000000 asid = 0x8000  PASS
  ASID write = 0xffff00000000000 readback = 0xffff00000000000 asid = 0xffff  PASS

MODE WARL probe (write MODE=15, reserved):
  write            = 0xf000000000000000
  readback         = 0x0
  MODE field       = 0
  ASID field       = 0x0
  PPN field        = 0x0

traps observed = 0

COMPLETION MARKER: satp-asid run finished
RESULT: PASS
```
