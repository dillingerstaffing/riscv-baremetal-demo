<!-- PROOF-HEADER
Checks: 8
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: satp MODE=Bare write/readback with nonzero PPN, direct physical access (S-mode)

## What was built

`src/satp-bare/`: a bare-metal RISC-V program that pins down what
`satp.MODE=0` (Bare) means on RV64, on the QEMU `virt` board. Two
files, sharing only `src/boot.S` and `src/uart.c` with the other
demos. Exactly one mechanism is under test: the MODE field of
`satp`, and the consequence that Bare means no address translation.

- `bare_trap.S`: M-mode direct-mode trap entry. No traps are
  expected on the happy path (the only `satp` write uses MODE=0, so
  translation is never enabled). If a trap fires it records
  `mcause`/`mepc`/`mtval` into C-visible globals, bumps the trap
  counter, prints the trap record and `RESULT: FAIL`, and parks the
  hart in a `wfi` loop.
- `bare_main.c`: M-mode setup (reads the boot-time `satp`, writes a
  canary to a scratch physical word at `0x81000000` and reads it
  back, installs the M-mode handler, opens the address space with
  one PMP NAPOT R/W/X entry because S-mode is default-deny) then
  `mret`s with `mstatus.MPP=01` into the S-mode payload
  `bare_smode_test`, which performs the whole experiment in S-mode:
  1. `csrw satp` with MODE=0 (Bare), ASID=0, PPN=`0xdeadbea0`;
     `csrr` readback; the readback must equal the written value,
     i.e. the MODE field reads back 0, the written Bare encoding,
     with the nonzero PPN stored verbatim (Bare mode ignores the
     PPN for translation but the field holds the written value).
  2. S-mode load of the scratch word: with `satp` still holding
     Bare+PPN, Bare means the effective address is the physical
     address itself, so this load must return exactly the canary
     M-mode stored at the physical word. Had translation been
     active, the load would have walked the garbage PPN as a
     page-table root and faulted.
  3. S-mode store of a second canary to the same word and
     loadback: it must round-trip, proving S-mode stores land on
     the physical address under Bare.
  4. Restore `satp` to 0, then verdict: `traps observed` must be 0
     (reaching the completion marker proves it, since the handler
     parks on any trap); prints the completion marker and
     `RESULT: PASS`, then shuts the machine down via the virt
     test-device finisher (QEMU exits 0).
     Any failed check prints `RESULT: FAIL` and parks instead.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make satp-bare.elf` (added to `all` and `clean` in the
Makefile). Run: `qemu-system-riscv64 -machine virt -nographic -bios
none -kernel satp-bare.elf` (`make run-satp-bare`).

## Measured results

Environment: QEMU emulator 8.2.2 (Debian), `-machine virt`,
`-bios none`, `riscv64-unknown-elf-gcc` 13.2.0, `-O2`.

satp write/readback (identical in all 3 runs):

- write `0xdeadbea0` (MODE=0 Bare, ASID=0, PPN nonzero),
  readback `0xdeadbea0`
- MODE field readback = 0, ASID field = `0x0`,
  PPN field = `0xdeadbea0` (stored verbatim)

Physical access under satp=Bare+PPN (identical in all 3 runs):

- M-mode canary stored at `0x81000000` = `0xba5eba1100000001`, read
  back by M-mode before the drop: match, PASS
- S-mode load of the same address returned `0xba5eba1100000001`,
  exactly the M-mode physical canary: PASS
- S-mode store `0xba5eba1100000002` and loadback returned
  `0xba5eba1100000002`: PASS

Program checks: 8 total (7 `check()` calls in the S-mode payload
plus the M-mode canary stickiness check in `main`), 0 failures in
every run; every comparison matched, so 0 mismatches.

Trap counts: 0 in all 3 runs (`traps observed = 0`; the M-mode
handler would have printed a FAIL record and parked on any trap, so
the printed completion marker is the no-trap proof).

Run verdicts: `RESULT: PASS` in all 3 runs; QEMU exit code 0 in all
3 runs. The three raw logs are byte-identical (verified with `diff`).

## Limits

- This is QEMU 8.2.2 on the `virt` machine, not silicon: that a
  nonzero PPN reads back verbatim in Bare mode is this emulator's
  WARL implementation choice. The RISC-V privileged spec defines
  `satp` as WARL and permits an implementation to return a legalized
  value; real hardware may behave differently (the write/readback
  procedure, not the number, is the portable part).
- The module deliberately never enables a non-Bare MODE, so the
  interaction of the PPN with an active translation regime is
  outside this module's scope.
- Single hart; no interrupts enabled during the run.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/satp-bare/bare_main.c -o src/satp-bare/bare_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o satp-bare.elf src/boot.o src/uart.o src/satp-bare/bare_trap.o src/satp-bare/bare_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: satp-bare.elf has a LOAD segment with RWX permissions
```

## Raw run logs (3 runs, byte-identical)

### run1.log
```

========================================
satp MODE=Bare write/readback + direct
physical access (S-mode)
========================================

satp at boot (M-mode read) = 0x0
M-mode canary stored at 0x81000000 = 0xba5eba1100000001  PASS
setup complete; dropping to S-mode...
in S-mode; satp MODE=Bare experiment begins

satp write/readback (MODE=Bare, nonzero PPN):
  write            = 0xdeadbea0
  readback         = 0xdeadbea0
  MODE field       = 0
  ASID field       = 0x0
  PPN field        = 0xdeadbea0

physical access under satp=Bare+PPN:
  S-mode load of physical word = 0xba5eba1100000001  (M-mode canary = 0xba5eba1100000001)  PASS
  S-mode store/loadback          = 0xba5eba1100000002  PASS

traps observed = 0

COMPLETION MARKER: satp-bare run finished
RESULT: PASS
```

### run2.log
```

========================================
satp MODE=Bare write/readback + direct
physical access (S-mode)
========================================

satp at boot (M-mode read) = 0x0
M-mode canary stored at 0x81000000 = 0xba5eba1100000001  PASS
setup complete; dropping to S-mode...
in S-mode; satp MODE=Bare experiment begins

satp write/readback (MODE=Bare, nonzero PPN):
  write            = 0xdeadbea0
  readback         = 0xdeadbea0
  MODE field       = 0
  ASID field       = 0x0
  PPN field        = 0xdeadbea0

physical access under satp=Bare+PPN:
  S-mode load of physical word = 0xba5eba1100000001  (M-mode canary = 0xba5eba1100000001)  PASS
  S-mode store/loadback          = 0xba5eba1100000002  PASS

traps observed = 0

COMPLETION MARKER: satp-bare run finished
RESULT: PASS
```

### run3.log
```

========================================
satp MODE=Bare write/readback + direct
physical access (S-mode)
========================================

satp at boot (M-mode read) = 0x0
M-mode canary stored at 0x81000000 = 0xba5eba1100000001  PASS
setup complete; dropping to S-mode...
in S-mode; satp MODE=Bare experiment begins

satp write/readback (MODE=Bare, nonzero PPN):
  write            = 0xdeadbea0
  readback         = 0xdeadbea0
  MODE field       = 0
  ASID field       = 0x0
  PPN field        = 0xdeadbea0

physical access under satp=Bare+PPN:
  S-mode load of physical word = 0xba5eba1100000001  (M-mode canary = 0xba5eba1100000001)  PASS
  S-mode store/loadback          = 0xba5eba1100000002  PASS

traps observed = 0

COMPLETION MARKER: satp-bare run finished
RESULT: PASS
```
