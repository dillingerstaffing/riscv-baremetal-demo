<!-- PROOF-HEADER
Checks: 4123
Mismatches: 0
Checksum: 0x7ebc94761e0d7274
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: PMP TOR exclusive-top-bound test

## What was built

`src/pmp-tor-top/`: a bare-metal M-mode RISC-V program that proves
a TOR PMP entry matches the half-open range
`[pmpaddr[i-1], pmpaddr[i])`: an `lbu` at the last byte inside the
denied range traps with the load access fault, while an `lbu` at
exactly the top bound matches no entry and completes under M-mode
default-allow with no trap. This is the third PMP boundary sibling,
alongside the done `src/pmp-tor` (bottom bound of a TOR entry) and
`src/pmp-napot-match` (NAPOT match). Three files, sharing only
`src/boot.S` and `src/uart.c` with the other demos.

- `tor_top_main.c`: UART bring-up, controls, PMP programming, the
  two boundary probes, a quiet window, the lock-persistence check,
  and the PASS/FAIL verdict. Each probe is a single inline-asm
  block so the instruction layout is exact (see below).
- `tor_top_trap.S`: minimal M-mode trap entry. mscratch points at
  the 8-word `tor_top_save` array; on entry it swaps t0, records
  mcause/mepc/mtval, bumps the total-trap counter, loads mepc from
  the resume address the test stored, flags the trap seen,
  restores t0/t1, and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three byte-identical run logs.

## Why the locked deny is entry 1, not entry 0

Entry 0's TOR range bottom is fixed at address 0: entry 0 in TOR
mode covers `[0, pmpaddr0)`. A locked deny entry 0 would therefore
deny the program's own code, the UART, and the test finisher, and
no probe could run. The top bound under test belongs to entry 1,
the locked entry: entry 0 is TOR over `[0, base)` with full
permissions, unlocked (`pmpcfg0 = 0x880F`: entry 0 = TOR + R|W|X,
entry 1 = L + TOR, R = W = X = 0); entry 1 is TOR over
`[base, base + 4 KiB)`, the locked deny. The fundamental truth
under test, the half-open top bound, is identical either way: an
access at exactly `pmpaddr1` does not match entry 1.

## Why the locked entry needs no MPRV

A locked entry is checked against M-mode accesses, so the probes
run as plain M-mode loads with no MPRV involved. The privileged
spec grants M-mode default-allow for accesses that match no entry
when at least one entry is locked. Entry 0's unlocked byte is
cleared (`csrw pmpcfg0, 0`) right after programming, which also
verifies the per-entry lock rule (entry 1's byte stays `0x88`,
entry 0's clears to `0x00`), so every probe runs with only the
locked entry programmed and the top-bound probe's clean completion
is a direct measurement of default-allow, not of an allow entry.

## Two lock-imposed design facts

The lock shapes the program in two ways, and both are turned into
checks rather than worked around.

1. The lock makes `pmpaddr1` and `pmpcfg0`'s entry-1 byte
   read-only until reset, and for TOR it also pins `pmpaddr0`
   (entry 1's bottom bound), so the programmed values cannot be
   restored to their boot values. The end-of-run step attempts the
   restore writes and asserts the readbacks are still the locked
   values (`pmpaddr0 = 0x20000c00`, `pmpaddr1 = 0x20001000`,
   `pmpcfg0 = 0x8800`) bit-for-bit, which is exactly the lock's
   advertised behavior.
2. The locked deny entry makes the 4 KiB region unreadable for
   the rest of the run, so "memory unchanged after the trap" is
   evidenced by the three things that remain observable: the
   faulting load's destination keeps its poison value (a faulting
   access never commits), the sentinel byte at exactly the top
   bound (outside the denied range) reads back bit-identical, and
   the control readback proved the whole region correct before
   programming.

## Controls (what the fault is compared against)

1. Before programming, the whole 4 KiB region is written with a
   byte pattern and read back byte-for-byte (4096 checks): proves
   every address in the probed range is good RAM, so the later
   fault comes from the PMP check and not from a bad address.
2. A layout guard runs before programming: `tor_top_save` (the
   trap handler's save area) must end at or below the region base;
   the `checks`/`fails` counters themselves (zero-initialized,
   hence `.bss`) must not alias the probed range or the sentinel
   byte, since the linker is free to place them right after the
   region array; and the live stack pointer must be more than
   1 KiB above the region end, so neither the handler, the
   counters, nor the C stack can touch the denied region or the
   sentinel. All values are run-time constants, identical every
   run (`tor_top_save=0x80002000 sp=0x80007f90`).
3. The sentinel byte `0xa5` is stamped at exactly the top bound
   (outside the region that will be denied) and read back before
   programming: T2 verifies a real value, not merely the absence
   of a trap.
4. After the probes, a quiet window of 10000 ordinary M-mode
   `mstatus` reads with the entry still programmed must show 0
   new traps, proving the entry disturbs nothing but the probed
   region.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

All three runs byte-identical, exit code 0.

| probe | seen | mcause | mepc | mtval | resume-4 | value |
|---|---|---|---|---|---|---|
| t1 lbu @0x80003fff (last byte inside) | 1 | 0x5 | 0x800006e2 | 0x80003fff | 0x800006e2 | poison kept |
| t1 neighbor sentinel @0x80004000 | - | - | - | - | - | 0xa5 |
| t2 lbu @0x80004000 (exclusive top bound) | 0 | - | - | - | - | 0xa5 |

(The mcause/mepc/mtval printed on the no-trap line are stale
leftovers from the trap in `tor_top_save`; `seen=0` is the verdict
and only it is checked.)

- `config:` lines confirm `pmpaddr0=0x20000c00`
  (`= 0x80003000 >> 2`, the region base) and
  `pmpaddr1=0x20001000` (`= 0x80004000 >> 2`, the top bound under
  test) and `pmpcfg0=0x880f` read back exactly as written; after
  the clear attempt `pmpcfg0=0x8800` (entry 1's byte locked at
  `0x88`, entry 0's byte cleared).
- `mepc` equals the faulting instruction on the trap. The
  disassembly was inspected: the T1 `lbu` sits at `0x800006e2`
  with the resume label at `0x800006e6`; the T2 `lbu` sits at
  `0x80000868` with resume at `0x8000086c`. Both are 4-byte
  instructions (`.option norvc`) immediately before the resume
  point, so the faulting access is always at `resume - 4`, and
  the program re-checks `mepc == resume - 4` on the trapped
  probe.
- `mtval` equals the faulting data address `0x80003fff`,
  matching the spec's rule that mtval carries the faulting
  address for access faults.
- The faulting `lbu` keeps the poison `0xdeadbeefdeadbeef` in
  the destination: a faulting load never writes it.
- The sentinel byte at the top bound reads back `0xa5` after
  the trap: the fault had no side effect on the adjacent byte.
- The top-bound `lbu` returns `0xa5` with 0 traps: no entry
  matched the half-open range's top bound, and M-mode
  default-allow completed the access.
- Quiet window: traps `1 -> 1`, no unexpected traps.
- Lock persistence: restore writes attempted, readbacks still
  `pmpaddr0=0x20000c00`, `pmpaddr1=0x20001000`, and
  `pmpcfg0=0x8800` bit-for-bit; the locked values cannot be
  changed until reset, as specified.
- `mstatus` restored to the boot value `0xa00000000` exactly.
- `Checks: 4123`, `Mismatches: 0`,
  `Checksum: 0x7ebc94761e0d7274`, `Environment: QEMU 8.2.2`,
  `Verdict: PASS`.

## One defect found and fixed during development

Recorded here because it changed what was verified.

The first build declared the probed region as
`static volatile unsigned char scratch[4096]`, and the top-bound
sentinel was written to `end = start + 4096`. The run reported
`Mismatches: 165` with no `FAIL:` lines printed, and
165 = 0xa5 = the sentinel value. `riscv64-unknown-elf-nm` showed
why: zero-initialized globals go in `.bss` (not `.data`), and the
linker had placed `fails` at exactly `0x80004000` (`end`) and
`checks` at `0x80004008`, so the sentinel write clobbered the
`fails` counter itself. Every real check had passed; only the
counter was corrupt. The fix owns the sentinel byte explicitly:
the region is now `region[4097]`, so `end == &region[4096]` is a
byte the linker cannot give to anything else, and a layout guard
asserts the `checks`/`fails` counters lie outside the probed
range. The rerun passed with 0 mismatches.

## Build

`bench-logs/build.log` captures the full build: GCC 13.2.0,
`-O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie
-fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`. The
only diagnostic is the repository's usual RWX LOAD segment
warning from the shared linker script. The module also ships a
Makefile stanza (`TOPT_SRCS`/`TOPT_OBJS`, `pmp-tor-top.elf` and
`run-pmp-tor-top` targets) appended at the end of the Makefile.
