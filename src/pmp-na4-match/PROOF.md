<!-- PROOF-HEADER
Checks: 21
Mismatches: 0
Checksum: 0xb3bbd7eb5f7a5e74
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: PMP locked-NA4 match-behavior test

## What was built

`src/pmp-na4-match/`: a bare-metal M-mode RISC-V program that
programs PMP entry 0 as a locked NA4 deny entry
(`pmpaddr0 = 0x20000800`, `pmpcfg0` byte `0x90`: L = 1, A = NA4,
R = W = X = 0) over exactly the 4-byte word at `0x80002000`, and
proves the match boundary: an `lbu` inside the word traps with
the load access fault, while an `lbu` one byte past the word
matches no entry and completes under M-mode default-allow with
no trap. This is the third PMP addressing mode, alongside the
done `src/pmp-tor` (TOR match) and `src/pmp-napot-match`
(NAPOT match) modules. Three files, sharing only `src/boot.S`
and `src/uart.c` with the other demos.

- `na4_main.c`: UART bring-up, controls, PMP programming, the
  two boundary probes, a quiet window, the lock-persistence
  check, and the PASS/FAIL verdict. Each probe is a single
  inline-asm block so the instruction layout is exact (see below).
- `na4_trap.S`: minimal M-mode trap entry. mscratch points at the
  8-word `na4_save` array; on entry it swaps t0, records
  mcause/mepc/mtval, bumps the total-trap counter, loads mepc from
  the resume address the test stored, flags the trap seen,
  restores t0/t1, and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three byte-identical run logs.

## Why the locked entry needs no MPRV

An unlocked entry is not checked against M-mode accesses, which
is why `src/pmp-napot-match` ran its probes with
`mstatus.MPRV = 1` and `MPP = S`. A locked entry is checked
against M-mode too, so the probes here are plain M-mode loads:
no MPRV, no MPP juggling, and no second allow entry. The
privileged spec grants M-mode default-allow for accesses that
match no entry whenever at least one entry is locked, so the
canary+4 probe's clean completion is a direct measurement of
that rule, not of an allow entry I placed.

## Two lock-imposed design facts

The lock bit shapes the program in two ways, and both are
turned into checks rather than worked around.

1. The lock makes `pmpcfg0` and `pmpaddr0` read-only until
   reset, so the programmed values cannot be restored to their
   boot values. The end-of-run step attempts the restore
   writes and asserts the readback is still the locked values
   (`pmpaddr0 = 0x20000800`, `pmpcfg0 = 0x90`) bit-for-bit,
   which is exactly the lock's advertised behavior (the
   `pmp-lock-bit` module proved the same read-only property
   with all-ones writes).
2. The locked deny entry makes the canary word unreadable for
   the rest of the run, so "memory unchanged after the trap" is
   evidenced by the three things that remain observable: the
   faulting load's destination keeps its poison value (a
   faulting access never commits), the neighbor word at
   canary+4 reads back bit-identical, and the control readback
   proved the word correct before programming.

## Controls (what the fault is compared against)

1. Before programming, both scratch words are written with
   known 4-byte patterns in M-mode and read back: proves the
   probed addresses are good RAM, so the later fault comes from
   the PMP check and not from a bad address. The stores are
   4-byte to match the probe word width, so the words never
   overlap. The canary+4 readback verifies a real value (the
   neighbor word's low byte `0x6c`), not merely the absence of
   a trap.
2. A layout guard runs before programming: `na4_save` (the trap
   handler's save area) must end at or below `0x80002000`, and
   the live stack pointer must be more than 1 KiB above
   `0x80002000`, so neither the handler nor the C stack can
   touch the denied word. Both values are run-time constants,
   identical every run (`na4_save=0x80000ee8 sp=0x80004eb0`).
3. After the probes, a quiet window of 10000 ordinary M-mode
   `mstatus` reads with the entry still programmed must show 0
   new traps, proving the entry disturbs nothing but the
   probed word.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

All three runs byte-identical, exit code 0.

| probe | seen | mcause | mepc | mtval | resume-4 | value |
|---|---|---|---|---|---|---|
| t1 lbu @0x80002000 (byte inside word) | 1 | 0x5 | 0x80000524 | 0x80002000 | 0x80000524 | poison kept |
| t1 neighbor word @0x80002004 | - | - | - | - | - | 0x9f8e7d6c |
| t2 lbu @0x80002004 (first byte outside) | 0 | - | - | - | - | 0x6c |

(The mcause/mepc/mtval printed on the no-trap line are stale
leftovers from the trap in `na4_save`; `seen=0` is the verdict
and only it is checked.)

- `config:` lines confirm `pmpaddr0=0x20000800`
  (`= 0x80002000 >> 2`, checked in C against the shift) and
  `pmpcfg0=0x90` read back exactly as written.
- `mepc` equals the faulting instruction on the trap. The
  disassembly was inspected: the `lbu` sits at `0x80000524`
  with the resume label at `0x80000528`; the t2 `lbu` sits at
  `0x80000696` with resume at `0x8000069a`. Both are 4-byte
  instructions (`.option norvc`) immediately before the resume
  point, so the faulting access is always at `resume - 4`, and
  the program re-checks `mepc == resume - 4` on the trapped
  probe.
- `mtval` equals the faulting data address `0x80002000`,
  matching the spec's rule that mtval carries the faulting
  address for access faults.
- The faulting `lbu` keeps the poison `0xdeadbeefdeadbeef` in
  the destination: a faulting load never writes it.
- The neighbor word reads back `0x9f8e7d6c` after the trap:
  the fault had no side effect on the adjacent word.
- The canary+4 `lbu` returns `0x6c`, the low byte of the
  neighbor word's known pattern, with 0 traps: no entry
  matched, M-mode default-allow completed the access.
- Quiet window: traps `1 -> 1`, no unexpected traps.
- Lock persistence: restore writes attempted, readbacks still
  `pmpaddr0=0x20000800` and `pmpcfg0=0x90` bit-for-bit; the
  locked values cannot be changed until reset, as specified.
- `mstatus` restored to the boot value `0xa00000000` exactly.
- `Checks: 21`, `Mismatches: 0`,
  `Checksum: 0xb3bbd7eb5f7a5e74`, `Environment: QEMU 8.2.2`,
  `Verdict: PASS`.

## One defect found and fixed during development

Recorded here because it changed what was verified.

The first build used `lw` in the 32-bit memory readback
helper. The neighbor pattern `0x9f8e7d6c` has bit 31 set, so
`lw` sign-extended it to `0xffffffff9f8e7d6c` and two checks
failed (the control readback and the post-trap neighbor
integrity check). The helper now uses `lwu`; the checked
equality is against the true word value. The canary pattern
`0x41b2c3d4` has bit 31 clear, which is why its checks passed
before the fix.

## Build

`bench-logs/build.log` captures the full build: GCC 13.2.0,
`-O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie
-fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`. The
only diagnostic is the repository's usual RWX LOAD segment
warning from the shared linker script.
