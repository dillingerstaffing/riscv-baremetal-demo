<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: PMP TOR boundary test (backlog item 103)

## What was built

`src/pmp-tor/`: a bare-metal RISC-V program that programs two PMP
entries in TOR (top-of-range) mode forming one exact boundary and
verifies the boundary byte by byte: the last byte below the boundary
reads cleanly, the first byte of the denied region traps. Four files,
about 330 lines total, sharing only `src/boot.S` and `src/uart.c` with
the other demos.

- `pmt_main.c`: UART bring-up, PMP programming, lock verification, two
  controls, the allowed-side probe, the denied-side probe, and the
  PASS/FAIL verdict. Each probe is a single inline-asm block so the
  instruction layout is exact (see below).
- `pmt_trap.S`: minimal M-mode trap entry. mscratch points at the
  7-word `pmt_save` array; on entry it swaps t0, records
  mcause/mepc/mtval, loads mepc from the resume address the test stored,
  flags the trap seen, restores t0/t1, and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make pmp-tor.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel pmp-tor.elf`
(or `make run-pmp-tor`).

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- `pmpaddr0 = 0x20000800`, `pmpaddr1 = 0x20000c00`: in TOR mode each
  entry's region runs from the previous pmpaddr to its own, so entry 0
  covers `[0x0, 0x80002000)` and entry 1 covers
  `[0x80002000, 0x80003000)` (4 KiB, the `scratch` buffer, which is 4 KiB
  aligned by construction).
- `pmpcfg0 = 0x880f`: entry 0 byte = A=TOR (0x08), R=W=X=1, unlocked;
  entry 1 byte = L=1 (0x80), A=TOR (0x08), R=W=X=0. The lowest-numbered
  matching entry wins, so the boundary between the two entries is exact
  to the byte.
- The L (lock) bit on entry 1 is required because this test runs in
  M-mode, and unlocked PMP entries are not checked against M-mode
  accesses (RISC-V privileged spec, PMP section). Locking was verified,
  not assumed: a `csrw pmpcfg0, 0` after programming leaves entry 1's
  byte at `0x88` while entry 0's unlocked byte clears to `0x00`,
  matching the per-entry lock rule. The boundary probes run after that
  clear, proving entry 0's permissions were never load-bearing for the
  denial.

## Controls (what the fault is compared against)

1. Before programming the PMP entries, the scratch buffer is written
   with a byte pattern (`i & 0xFF`) and read back, and a sentinel
   `0x5A` is written to the byte just below the boundary
   (`0x80001FFF`, BSS padding under the 4 KiB-aligned scratch) and read
   back: proves both addresses are good RAM, so the later fault comes
   from the PMP check and not from a bad address.
2. After programming, a second buffer (`probe_outside`, elsewhere in
   `.bss`) is written and read back: proves the denied entry is narrow
   and the rest of the address space still works.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

| probe | seen | mcause | mepc | mtval | resume-8 | byte |
|---|---|---|---|---|---|---|
| lbu @0x80001fff (last allowed) | 0 | 0x0 | 0x0 | 0x0 | 0x80000272 | 0x5a |
| lbu @0x80002000 (first denied) | 1 | 0x5 | 0x80000272 | 0x80002000 | 0x80000272 | - |

Identical across all three runs (byte-identical output apart from the
`timeout` kill line). Every program-internal check passed; all three
runs print `RESULT: PASS`.

What each column means:

- The allowed-side `lbu` at `0x80001FFF` completes with no trap
  (`seen=0`) and returns the `0x5A` sentinel written before programming:
  the byte below the boundary is genuinely readable, not merely
  non-faulting.
- The denied-side `lbu` at `0x80002000` traps with `mcause` 5 = load
  access fault, the exact code the privileged spec's trap table
  assigns. Ground truth is the spec table, and the observed value
  matches it.
- `mepc` equals the address of the faulting instruction. This is
  checked, not eyeballed: the probe's asm block lays out `auipc` (4
  bytes, never compressed), then the `lbu` (4 bytes; t1 is not a
  compressible register), then a 4-byte `sd` spilling the loaded byte,
  then the resume label. The trap handler resumes at the label, so the
  faulting instruction is always at `resume - 8`. The disassembly was
  inspected to confirm the layout (auipc at 0x8000026e, lbu at
  0x80000272, sd at 0x80000276, resume at 0x8000027a), and the program
  re-checks `mepc == resume - 8` on every run.
- `mtval` = 0x80002000 = the faulting address (start of the scratch
  buffer), matching the spec's rule that mtval carries the faulting
  address for access faults.

## One defect found and fixed during development

Recorded here because it changed what was verified.

Wrong config byte (mine). The first version programmed entry 1's byte
as `0x89`, intending L + TOR + no permissions, but `0x89` =
`0x80 | 0x08 | 0x01`: bit 0 is the R bit, so the entry was locked TOR
*readable*, and both probes passed without any trap. The readback and
lock checks were self-consistent (they compared against the same wrong
constant), so they did not catch it. Caught by reading QEMU 8.2.2's
`pmp_hart_has_privs` (`target/riscv/pmp.c`): the match loop ANDs the
entry's config byte into the allowed privileges, and with R set the
load was legitimately allowed. Fixed to `0x88` (L + TOR, R=W=X=0);
the denied probe then trapped exactly as specified. Lesson applied:
config-byte constants are now derived bit by bit in the comment next to
each `#define`, and the "denied" probe failing to trap is treated as a
test bug first, an emulator question second.

(The `&&label` lesson from the sibling module was applied from the
start: the resume address is taken inside the asm block with
`la t0, 1f` against a numeric local label, resolved exactly by the
assembler.)

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's PMP model on the `virt` machine, not real
  silicon. The trap code 5 is architectural (spec table), but the TOR
  boundary matching and the locked-entry M-mode check are only as good
  as QEMU's implementation of those rules (cross-checked against QEMU's
  own `target/riscv/pmp.c` during debugging, which is how the 0x89/0x88
  defect was found).
- Only entries 0-1, only TOR, only a 4 KiB region, only hart 0, only
  M-mode. Stores, instruction-fetch denial, and S/U-mode behavior are
  not tested; the module is deliberately that small.
- mepc/mtval addresses are specific to this binary's layout; the
  invariants that transfer are `mepc == resume - 8` and
  `mtval == faulting address`, re-checked by the program on every run.

## Reproduction

```
make pmp-tor.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel pmp-tor.elf
```

Toolchain used: `riscv-none-elf-gcc` 15.2.0 (xpack), QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; QEMU is terminated by `timeout` afterwards
because the bare-metal image never exits QEMU on its own).
