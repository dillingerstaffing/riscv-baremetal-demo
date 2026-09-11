<!-- PROOF-HEADER
Checks: 17
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: PMP lock-bit persistence test (backlog item: riscv pmp-lock-bit)

## What was built

`src/pmp-lock-bit/`: a bare-metal RISC-V program that programs PMP entry
0 as a locked NAPOT region with no permissions and verifies the two
properties the RISC-V privileged specification gives the L (lock) bit.
Four files, about 350 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos.

- `plb_main.c`: UART bring-up, PMP programming, lock-persistence
  probes (all-ones writes to `pmpcfg0` and `pmpaddr0`), two controls,
  the M-mode load test, and the PASS/FAIL verdict. The faulting load is
  a single inline-asm block so the instruction layout is exact (see
  below).
- `plb_trap.S`: minimal M-mode trap entry. mscratch points at the
  7-word `plb_save` array; on entry it swaps t0, records
  mcause/mepc/mtval, loads mepc from the resume address the test stored,
  increments the trap counter, restores t0/t1, and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make pmp-lock-bit.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel pmp-lock-bit.elf`
(or `make run-pmp-lock-bit`).

This module isolates the lock-bit mechanism. Denial trap codes for
loads and stores inside a locked region are covered separately by
`src/pmp/` (backlog item 25).

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- PMP entry 0: `pmpaddr0 = 0x200009ff`, which decodes as NAPOT covering
  exactly `[0x80002000, 0x80003000)` (4 KiB, the `scratch` buffer, which
  is 4 KiB aligned by construction).
- `pmpcfg0 = 0x98`: entry 0 byte = L=1, A=NAPOT (0x18), R=W=X=0.
  Entries 1-7 are A=OFF (no match) at programming time.
- The L (lock) bit is what makes the M-mode load test possible at all:
  unlocked PMP entries are not checked against M-mode accesses (RISC-V
  privileged spec, PMP section).

## Property 1: lock persistence (the L bit freezes entry 0)

With entry 0's L bit set, the privileged spec says writes to the
entry's config and address registers are ignored until reset. The test
attempts the most hostile write, all-ones (`0xFFFFFFFFFFFFFFFF`), to
both registers and reads them back:

- `pmpcfg0` after the all-ones write: `0xffffffffffffff98`. Entry 0's
  byte is `0x98`, unchanged. Entries 1-7 are unlocked and did accept
  the write (they read back `0xFF`); the check covers only the locked
  entry, and the full readback is printed so nothing is hidden.
- `pmpaddr0` after the all-ones write: `0x200009ff`, bit-identical to
  the programmed value.

Ground truth is the spec's lock rule; the observed readbacks match it.

## Property 2: the locked entry denies M-mode too

A load from inside the locked region traps exactly once per run:

| test | traps | mcause | mepc | mtval | resume-4 |
|---|---|---|---|---|---|
| load | 1 | 0x5 | 0x80000540 | 0x80002000 | 0x80000540 |

Identical across all three runs. Every program-internal check passed
(17 checks, 0 failures); all three runs print `RESULT: PASS`.

What each column means:

- `mcause` 5 = load access fault: the exact code the privileged spec's
  trap table assigns. Ground truth is the spec table, and the observed
  value matches it.
- `mepc` equals the address of the faulting instruction. This is
  checked, not eyeballed: the asm block lays out `la t0, 1f` (auipc +
  addi, resolved by the assembler to the resume label at 0x80000544),
  then the resume-address and counter stores, then `auipc` (4 bytes,
  never compressed) at 0x8000053c, then the `lw` (4 bytes; t1 is not a
  compressible register) at 0x80000540, then label 1 at 0x80000544.
  The trap handler resumes at the label, so the faulting instruction is
  always at `resume - 4`. The disassembly was inspected to confirm the
  layout (`la t0, 1f` targets 0x80000544; lw at 0x80000540).
- `mtval` = 0x80002000 = the faulting address (start of the scratch
  buffer), matching the spec's rule that mtval carries the faulting
  address for access faults.

## Controls (what the results are compared against)

1. Before programming the PMP entry, the scratch buffer is written with
   `0xA5..0xA8` and read back: proves the address is good RAM, so the
   later fault comes from the PMP check and not from a bad address.
2. After the lock-persistence probes, a second buffer
   (`probe_outside`, at `0x80001038`, outside the region) is written
   and read back: proves the entry is narrow and the rest of the
   address space still works.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's PMP model on the `virt` machine, not real
  silicon. The trap code 5 and the lock-write-ignore rule are
  architectural (spec text), but the check that a locked entry denies
  M-mode is only as good as QEMU's implementation of that rule.
- Only entry 0, only NAPOT, only a 4 KiB region, only hart 0, only
  M-mode. Instruction-fetch denial (X=0) and S-mode behavior are not
  tested; the module is deliberately that small.
- No throughput is measured; there is no genuine throughput number to
  report, so none appears in the header above.
- mepc/mtval addresses are specific to this binary's layout; the
  invariant that transfers is `mepc == resume - 4` and
  `mtval == faulting address`, re-checked by the program on every run.

## Reproduction

```
make pmp-lock-bit.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel pmp-lock-bit.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; QEMU is terminated by `timeout` afterwards
because the bare-metal image never exits QEMU on its own).
