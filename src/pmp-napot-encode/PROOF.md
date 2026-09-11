<!-- PROOF-HEADER
Checks: 9
Mismatches: 0
Checksum: 0x7abdfe9a3880db7b
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: PMP NAPOT encoding write/readback (backlog item "riscv pmp-napot-encode")

## What was built

`src/pmp-napot-encode/`: a bare-metal RISC-V program that checks
the NAPOT encoding of a PMP address register directly. A naturally
aligned power-of-two region is encoded as
`pmpaddr = (base >> 2) | ((size >> 3) - 1)`: the address above the
region's granularity bits, plus a tail of trailing ones whose count
names the size. The run writes three such patterns to `pmpaddr0`
and requires each to read back exactly as written, then programs
`pmpcfg0` entry 0 with A = NAPOT and requires the A field to read
back as 3. M-mode only, no traps expected: every entry keeps L = 0
for the whole run, and unlocked PMP entries do not restrict M-mode
at all, so the probe cannot fault. `pmpaddr0`, `pmpaddr1`, and
`pmpcfg0` are restored to their boot readbacks at the end, and the
restore itself is checked. Two files, sharing only `src/boot.S`
and `src/uart.c` with the other demos. Exactly one mechanism is
under test: the encoding of a NAPOT region in `pmpaddr` and of the
address-matching mode in the `pmpcfg` A field.

- `pne_main.c`: records boot `pmpaddr0`/`pmpaddr1`/`pmpcfg0`; writes
  `0x200009ff` (4 KiB at `0x80002000`, 9 trailing ones),
  `0x20101fff` (64 KiB at `0x80400000`, 13 trailing ones),
  `0x2201ffff` (1 MiB at `0x88000000`, 17 trailing ones) to
  `pmpaddr0` requiring exact readback; writes `pmpcfg0 = 0x18`
  (entry 0: A = NAPOT) requiring the A field (bits 4:3) to read 3
  and the entry-0 byte to read `0x18` exactly; writes
  `pmpcfg0 = 0x1f` (entry 0: NAPOT + R + W + X, L clear) requiring
  the byte to read `0x1f` exactly, showing the permission bits
  coexist with the address-matching field; restores all three CSRs
  to boot values with each restore readback checked against boot.
  Prints the written-vs-readback table, computes an FNV-1a checksum
  over the recorded values, and reports PASS/FAIL. On PASS it
  writes `0x5555` to the virt test-device finisher so the QEMU
  process exits 0; on FAIL it parks the hart without touching the
  finisher.

## How it was verified

- Built once with the repo Makefile (`make pmp-napot-encode.elf`,
  `src/boot.S` first in the link order so `_start` lands at
  `0x80000000`, the address QEMU's `-kernel` loader starts at):
  build log in `bench-logs/build.log`.
- Ran 3 times under QEMU 8.2.2 (`-machine virt -nographic -bios
  none -kernel`); raw console logs in `bench-logs/run1.log`,
  `run2.log`, `run3.log`. All three runs exit 0 and are
  byte-identical.
- Each run executes 9 checks and records 0 mismatches. The FNV-1a
  checksum `0x7abdfe9a3880db7b` is identical across all three runs.

## The numbers

Written-vs-readback table (from `bench-logs/run1.log`, identical
in runs 2 and 3):

| CSR              | written   | readback  |
|------------------|-----------|-----------|
| boot pmpaddr0    | -         | 0x0       |
| boot pmpaddr1    | -         | 0x0       |
| boot pmpcfg0     | -         | 0x0       |
| pmpaddr0 (4 KiB) | 0x200009ff| 0x200009ff|
| pmpaddr0 (64 KiB)| 0x20101fff| 0x20101fff|
| pmpaddr0 (1 MiB) | 0x2201ffff| 0x2201ffff|
| pmpcfg0 (A=NAPOT)| 0x18      | 0x18      |
| pmpcfg0 (NAPOT+RWX)| 0x1f    | 0x1f      |
| restore pmpaddr0 | 0x0       | 0x0       |
| restore pmpaddr1 | 0x0       | 0x0       |
| restore pmpcfg0  | 0x0       | 0x0       |

Checks: 9, mismatches: 0, record checksum `0x7abdfe9a3880db7b`,
`RESULT: PASS` in all three runs. QEMU 8.2.2 honors the full NAPOT
patterns with no WARL legalization at these magnitudes: every
written bit reads back. A-field check: `0x18 & 0x18 == 0x18`
confirms A = 3 (NAPOT).

## Limits of verification

- Three sizes (4 KiB, 64 KiB, 1 MiB) on `pmpaddr0` only; larger
  regions and higher entry numbers are not exercised. The three
  patterns are each 30 bits or fewer, well within RV64 `pmpaddr`'s
  implemented range; behavior near `XLEN`'s top bits is not probed.
- The A field is checked on entry 0 of `pmpcfg0` only; the other
  fifteen entries' bytes are read as part of the whole-csr readback
  (`0x18` / `0x1f` masked to the entry-0 byte).
- No PMP permission enforcement is exercised here: no locked entry
  is ever programmed and no access fault is provoked. The question
  answered is only "does the encoding write and read back," which
  is what the module claims. Enforcement boundaries (locked NAPOT
  deny regions at 4 KiB and 64 KiB) were verified separately by
  the `pmp-napot-size` module.
- No WARL legalization was observed, so none is documented; if a
  future QEMU version legalizes these patterns, the byte-identical
  run logs here record what 8.2.2 did.
