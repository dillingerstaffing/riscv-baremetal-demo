<!-- PROOF-HEADER
Checks: 30324
Mismatches: 0
Checksum: 0x6214dda909b820e0
Throughput: 10000 amoadd.w in 31218975 / 33623175 / 28729980 mcycle across the 3 runs (TCG emulation timing, varies run to run)
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: amoadd.w read-modify-write semantics, single hart

## What was built

`src/amo-add-atomicity/`: a bare-metal RISC-V program, single hart,
M-mode, that exercises the `amoadd.w` atomic read-modify-write
instruction against two 4-byte aligned cells and checks the
instruction's contract point by point:

- Phase A: `cell_a` starts at `0x10000000`; 10000 iterations of
  `amoadd.w` with increment 1. Each iteration checks the returned value
  against the expected old value (`base + i`), so a return of the new
  value instead of the old, or any skipped/duplicated value, fails
  immediately. After the loop the code checks the closed-form
  arithmetic-series sum of all 10000 returned values
  (`N*base + N*(N-1)/2`), the exact final word (`base + 10000`), and a
  second, byte-by-byte reconstruction read of the same memory.
- Phase B: `cell_b` starts at `0x20000000`; 100 iterations of
  `amoadd.w` with increment 7, with the same per-iteration old-value
  check, sum invariant, final-word check (`base + 700`), and byte-wise
  readback.

A minimal M-mode trap handler (`aaa_trap.S`) counts traps and parks the
hart on the first one: `amoadd.w` on an aligned word in M-mode with no
interrupts enabled cannot fault here, so a printed `RESULT: PASS`
implies the trap count stayed zero; the C code also checks the counter
explicitly and prints it.

Four files, about 300 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos.

- `aaa_main.c`: UART bring-up, trap vector installation, aligned-access
  control, the two phases, cycle timing via `cycle`, the FNV-1a
  checksum over the final cell values, and the PASS/FAIL verdict.
- `aaa_trap.S`: minimal M-mode trap entry; increments the trap count,
  records mcause/mepc/mtval, parks the hart.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make amo-add-atomicity.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel amo-add-atomicity.elf`
(or `make run-amo-add-atomicity`).

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- QEMU: 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18), `-machine virt`,
  single hart (default).
- `amoadd.w` assembled directly (`-march=rv64imac_zicsr`); cells are
  4-byte aligned as the instruction requires.

## What was checked (per run, 10108 checks)

1. Control: plain aligned load/store roundtrip on both cells (1).
2. Phase A: 10000 old-value-return checks, one per `amoadd.w`.
3. Phase A: sum of the 10000 returned old values equals the closed-form
   arithmetic-series sum `10000 * 0x10000000 + 10000*9999/2` (1).
4. Phase A: final word equals `0x10000000 + 10000` (1).
5. Phase A: byte-wise reconstruction of the final word equals
   `0x10000000 + 10000` (1).
6. Phase B: 100 old-value-return checks, one per `amoadd.w`.
7. Phase B: sum of the 100 returned old values equals
   `100 * 0x20000000 + 7*100*99/2` (1).
8. Phase B: final word equals `0x20000000 + 700` (1).
9. Phase B: byte-wise reconstruction of the final word equals
   `0x20000000 + 700` (1).
10. Trap count equals 0 (1).

Three runs were executed (three separate QEMU boots); every check ran
in every boot, so the header's `Checks: 30324` is 10108 x 3.

## Results (verbatim from bench-logs/run1.log, run2.log, run3.log)

| run | phase A final | phase B final | traps | checksum | checks | mismatches | verdict |
|-----|---------------|---------------|-------|----------|--------|------------|---------|
| 1 | 0x10002710 | 0x200002bc | 0 | 0x6214dda909b820e0 | 10108 | 0 | PASS |
| 2 | 0x10002710 | 0x200002bc | 0 | 0x6214dda909b820e0 | 10108 | 0 | PASS |
| 3 | 0x10002710 | 0x200002bc | 0 | 0x6214dda909b820e0 | 10108 | 0 | PASS |

- `0x10002710` = `0x10000000 + 10000` (0x2710 = 10000 decimal).
- `0x200002bc` = `0x20000000 + 700` (0x2bc = 700 decimal).
- Cycle counts for the 10000 phase-A increments: 31218975, 33623175,
  28729980 mcycle. This is TCG emulation timing on the build machine,
  not hardware timing; it varies run to run and is reported only so a
  reader can see the measurement exists, not as a performance claim.

## Honest scope

A single hart issuing back-to-back `amoadd.w` cannot observe
contention, so this module does not and cannot prove atomicity under
multi-hart contention. What it verifies, on QEMU 8.2.2, is the
instruction's single-hart read-modify-write contract: each operation
returns the old value (checked on all 10100 operations), the final
value equals exactly base + N increments with no lost update, the
returned-value stream matches its closed-form sum, and zero traps
fire. Anything beyond that would be a claim this setup cannot make.
