<!-- PROOF-HEADER
Checks: 10000
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: LR/SC attempt-histogram experiment

## What was built

`src/lrsc-histogram/`: a bare-metal RISC-V program, single hart,
M-mode, that runs 10,000 iterations of an aligned `lr.w`/`sc.w` pair
on one 4-byte word with no contention. Each iteration retries the
pair until `sc.w` reports success (rd == 0) and counts how many
attempts that took; the program prints the attempts-to-success
histogram. The value stored in each iteration is the iteration index,
read back after the successful sc and checked, so all 10,000 stores
are verified by round-trip. A minimal M-mode trap handler
(`lrsc_trap.S`) records mcause/mepc/mtval into `lrsc_save` and halts
the hart; no trap is expected, and the program checks the handler's
seen flag as part of the verdict, so an unexpected trap would fail
the run instead of passing silently.

Three files, about 240 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos.

- `lrsc_main.c`: UART bring-up, trap vector installation, the 10,000
  iteration loop with the retry counter and histogram, per-store
  readback verification, and the PASS/FAIL verdict.
- `lrsc_trap.S`: minimal M-mode trap entry. mscratch points at the
  4-word `lrsc_save` array; on entry it swaps t0, records
  mcause/mepc/mtval, flags the trap seen, restores t0, and parks the
  hart in a wfi loop.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make lrsc-histogram.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -smp 1 -nographic -bios none -kernel lrsc-histogram.elf`
(or `make run-lrsc-histogram`).

## Construction detail

The retry loop is a single `asm volatile` block:

```
lr.w t1, 0(%1)
sc.w %0, %2, 0(%1)
```

Disassembly of the built ELF confirms the layout (lr at
0x800002e2, sc at 0x800002e6): the lr is immediately followed by the
sc with no instruction in between, which is exactly the shape the
reservation mechanism is defined over. The attempts counter and the
sc-result test live outside the atomic window, so they cannot
disturb the reservation. The C compiler is given no room to reorder
across the block (`"memory"` clobber, volatile).

## Configuration under test

- Hart: mhartid = 0, single hart (`-smp 1`), M-mode (QEMU boots the
  ELF straight into M-mode with `-bios none`).
- Target word: one 4-byte aligned static cell, accessed only through
  the lr/sc pair; nothing else runs on the hart, so there is no
  contention for the reservation.
- 10,000 iterations; stored value per iteration = the iteration index
  (0..9999), read back after each successful sc.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

Byte-identical across all three runs (the only log difference is the
pid in the timeout kill line).

| measurement | run 1 | run 2 | run 3 |
|---|---|---|---|
| histogram: attempts=1 | 10000 | 10000 | 10000 |
| histogram bins attempts>=2 | 0 | 0 | 0 |
| histogram sum | 10000 | 10000 | 10000 |
| max attempts in one iteration | 1 | 1 | 1 |
| stored-value readback errors | 0 | 0 | 0 |
| trap handler seen flag | 0 | 0 | 0 |
| RESULT | PASS | PASS | PASS |

What each value means:

- Every one of the 10,000 `lr.w`/`sc.w` pairs succeeded on its first
  attempt. On this machine (QEMU 8.2.2 `virt`, single hart, no
  contention) an aligned `sc.w` issued immediately after its `lr.w`
  never fails: the attempts-to-success histogram is a single spike
  at 1. This is the uncontended forward-progress ground truth for
  this pair shape on this emulator; any future contended experiment
  (second hart, intervening stores, interrupt between lr and sc)
  measures its deviation against this baseline.
- The per-iteration readback check confirms all 10,000 stores landed
  with the exact intended value (iteration index), so the histogram
  counts 10,000 real successful stores, not 10,000 sc return codes
  that were never validated against memory.
- The trap handler's seen flag stayed 0 on all runs: no trap fired
  during any of the 30,000 lr/sc instructions, as expected for
  aligned atomics on a single hart.

The program's PASS verdict covers its own accounting only:
histogram sums to 10,000, zero readback errors, zero traps. It does
not assert what any other machine must do.

## Limits of verification (read before citing numbers)

- This measures the behavior of QEMU 8.2.2's `virt` machine, an
  emulator. It is emulator behavior, not silicon. On real cores the
  reservation granularity and the conditions that clear a
  reservation differ; an sc can fail spuriously on real hardware
  where QEMU never fails it here.
- Only the uncontended single-hart case: one hart, one aligned word,
  nothing else executing, interrupts never enabled, lr immediately
  followed by sc. Any contention source (a second hart, a store to
  the reservation granule between lr and sc, an interrupt in the
  window) is outside this module; the histogram published here is
  the baseline those experiments would be compared against.
- Only word-width `lr.w`/`sc.w`; `lr.d`/`sc.d`, AMO arithmetic ops,
  and misaligned addresses are not tested (misaligned LR/SC is
  covered by `src/amo/`).

## Reproduction

```
make lrsc-histogram.elf
timeout 30 qemu-system-riscv64 -machine virt -smp 1 -nographic -bios none -kernel lrsc-histogram.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; QEMU is terminated by `timeout` afterwards
because the bare-metal image never exits QEMU on its own).
