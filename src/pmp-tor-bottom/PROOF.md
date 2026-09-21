<!-- PROOF-HEADER
Checks: 4125
Mismatches: 0
Checksum: 0xda9c5efc875beabd
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: PMP TOR inclusive-bottom-bound test

## What was built

`src/pmp-tor-bottom/`: a bare-metal M-mode RISC-V program that proves
a TOR PMP entry matches the half-open range
`[pmpaddr[i-1], pmpaddr[i])` with the bottom bound inclusive: an `lbu`
exactly at the bottom bound of a locked deny entry traps with the
load access fault, while an `lbu` one byte below the bottom bound
matches no entry and completes under M-mode default-allow with no
trap. This is the fourth PMP boundary sibling, alongside the done
`src/pmp-tor-top` (exclusive top bound of a locked TOR deny),
`src/pmp-tor` (bottom bound of a TOR entry), and
`src/pmp-napot-match` (NAPOT match). Three files, sharing only
`src/boot.S` and `src/uart.c` with the other demos.

- `tor_bot_main.c`: UART bring-up, controls, PMP programming, the
  two boundary probes, a quiet window, the lock-persistence check,
  and the PASS/FAIL verdict. Each probe is a single inline-asm
  block so the instruction layout is exact (see below).
- `tor_bot_trap.S`: minimal M-mode trap entry. mscratch points at
  the 8-word `tor_bot_save` array; on entry it swaps t0, records
  mcause/mepc/mtval, bumps the total-trap counter, loads mepc from
  the resume address the test stored, flags the trap seen,
  restores t0/t1, and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three byte-identical run logs.

## Why the locked deny is entry 1, not entry 0

Entry 0's TOR range bottom is fixed at address 0: entry 0 in TOR
mode covers `[0, pmpaddr0)`. A locked deny entry 0 would therefore
deny the program's own code, the UART, and the test finisher, and
no probe could run. The bottom bound under test is `pmpaddr0`
(`0x20000c00`, i.e. base `0x80003000`), which is entry 1's bottom
bound: entry 0 is TOR over `[0, base)` with full permissions,
unlocked (`pmpcfg0 = 0x880F`: entry 0 = TOR + R|W|X,
entry 1 = L + TOR, R = W = X = 0); entry 1 is TOR over
`[base, base + 4 KiB)`, the locked deny. The fundamental truth
under test, the half-open bottom bound, is identical either way:
an access at exactly `pmpaddr0` still matches entry 1.

## Why the locked entry needs no MPRV

A locked entry is checked against M-mode accesses, so the probes
run as plain M-mode loads with no MPRV involved. The privileged
spec grants M-mode default-allow for accesses that match no entry
when at least one entry is locked. Entry 0's unlocked byte is
cleared (`csrw pmpcfg0, 0`) right after programming, which also
verifies the per-entry lock rule (entry 1's byte stays `0x88`,
entry 0's clears to `0x00`), so every probe runs with only the
locked entry programmed and the below-bound probe's clean
completion is a direct measurement of default-allow, not of an
allow entry.

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
   access never commits), the sentinel byte one byte below the
   bottom bound (outside the denied range) reads back
   bit-identical, and the control readback proved the whole region
   correct before programming.

## Controls (what the fault is compared against)

1. Before programming, the whole 4 KiB region is written with a
   byte pattern and read back byte-for-byte (4096 checks): proves
   every address in the probed range is good RAM, so the later
   fault comes from the PMP check and not from a bad address.
2. A layout guard runs before programming: the region start is
   pinned to `0x80003000` (checked, not assumed) and 4 KiB
   aligned so `pmpaddr0 = start >> 2` is exact; `tor_bot_save`
   (the trap handler's save area) must end at or below the region
   base; the `checks`/`fails` counters themselves
   (zero-initialized, hence `.bss`) must not alias the probed
   range or the sentinel byte at `start - 1`, since the linker is
   free to place them right after the region array (and in this
   build `fails` lands exactly at `end`, which the guard
   explicitly allows); and the live stack pointer must be more
   than 1 KiB above the region end, so neither the handler, the
   counters, nor the C stack can touch the denied region or the
   sentinel. All values are run-time constants, identical every
   run (`start=0x80003000 tor_bot_save=0x80002000
   sp=0x80007f70`).
3. The sentinel byte `0x5a` is stamped one byte below the bottom
   bound (outside the region that will be denied) and read back
   before programming: T2 verifies a real value, not merely the
   absence of a trap.
4. After the probes, a quiet window of 10000 ordinary M-mode
   `mstatus` reads with the entry still programmed must show 0
   new traps, proving the entry disturbs nothing but the probed
   region.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

All three runs byte-identical, exit code 0.

| probe | seen | mcause | mepc | mtval | resume-4 | value |
|---|---|---|---|---|---|---|
| t1 lbu @0x80003000 (the bottom bound) | 1 | 0x5 | 0x80000726 | 0x80003000 | 0x80000726 | poison kept |
| t1 neighbor sentinel @0x80002fff | - | - | - | - | - | 0x5a |
| t2 lbu @0x80002fff (below the bound) | 0 | - | - | - | - | 0x5a |

(The mcause/mepc/mtval printed on the no-trap line are stale
leftovers from the trap in `tor_bot_save`; `seen=0` is the verdict
and only it is checked.)

- `config:` lines confirm `pmpaddr0=0x20000c00`
  (`= 0x80003000 >> 2`, the bottom bound under test) and
  `pmpaddr1=0x20001000` (`= 0x80004000 >> 2`) and `pmpcfg0=0x880f`
  read back exactly as written; after the clear attempt
  `pmpcfg0=0x8800` (entry 1's byte locked at `0x88`, entry 0's
  byte cleared).
- `mepc` equals the faulting instruction on the trap. The
  disassembly was inspected: the T1 `lbu` sits at `0x80000726`
  with the resume label at `0x8000072a`; the T2 `lbu` sits at
  `0x800008a6` with resume at `0x800008aa`. Both are 4-byte
  instructions (`.option norvc`) immediately before the resume
  point, so the faulting access is always at `resume - 4`, and
  the program re-checks `mepc == resume - 4` on the trapped
  probe.
- `mtval` equals the faulting data address `0x80003000`,
  matching the spec's rule that mtval carries the faulting
  address for access faults.
- The faulting `lbu` keeps the poison `0xdeadbeefdeadbeef` in
  the destination: a faulting load never writes it.
- The sentinel byte one byte below the bottom bound reads back
  `0x5a` after the trap: the fault had no side effect on the
  adjacent byte.
- The below-bound `lbu` returns `0x5a` with 0 traps: no entry
  matched below the half-open range's bottom bound, and M-mode
  default-allow completed the access.
- Quiet window: traps `1 -> 1`, no unexpected traps.
- Lock persistence: restore writes attempted, readbacks still
  `pmpaddr0=0x20000c00`, `pmpaddr1=0x20001000`, and
  `pmpcfg0=0x8800` bit-for-bit; the locked values cannot be
  changed until reset, as specified.
- `mstatus` restored to the boot value `0xa00000000` exactly.
- `Checks: 4125`, `Mismatches: 0`,
  `Checksum: 0xda9c5efc875beabd`, `Environment: QEMU 8.2.2`,
  `Verdict: PASS`.

## One defect found and fixed during development

Recorded here because it changed what was verified.

The first draft of the layout guard reused the sibling's
counter-alias form (`&fails < start || &fails > end`) with the
sentinel moved below the bound. `riscv64-unknown-elf-nm` showed
the linker had placed `fails` at exactly `0x80004000` (`end`),
which is outside both the denied range `[start, end)` and the
sentinel byte at `start - 1`, so it is harmless, but the strict
`> end` form would have failed the run for it. The guard now
asserts the counters avoid `[start - 1, end)`: `&x < start - 1 ||
&x >= end`, which accepts a counter at exactly `end` while still
rejecting any counter the sentinel stamp could clobber. The rerun
passed with 0 mismatches.

## Build

`bench-logs/build.log` captures the full build: GCC 13.2.0,
`-O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie
-fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`. The
only diagnostic is the repository's usual RWX LOAD segment
warning from the shared linker script. The module also ships a
Makefile stanza (`PBOT_SRCS`/`PBOT_OBJS`, `pmp-tor-bottom.elf` and
`run-pmp-tor-bottom` targets) appended at the end of the Makefile.
