# Proof: PMP no-access denial test (backlog item 25)

## What was built

`src/pmp/`: a bare-metal RISC-V program that programs PMP entry 0 as a
locked NAPOT region with no permissions and verifies that a load and a
store inside the region trap with the architecturally defined cause
codes. Three files, about 330 lines total, sharing only `src/boot.S`
and `src/uart.c` with the other demos.

- `pmp_main.c`: UART bring-up, PMP programming, lock verification, two
  control checks, the load test, the store test, and the PASS/FAIL
  verdict. Each test is a single inline-asm block so the instruction
  layout is exact (see below).
- `pmp_trap.S`: minimal M-mode trap entry. mscratch points at the
  7-word `pmp_save` array; on entry it swaps t0, records
  mcause/mepc/mtval, loads mepc from the resume address the test stored,
  flags the trap seen, restores t0/t1, and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make pmp.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel pmp.elf`
(or `make run-pmp`).

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- PMP entry 0: `pmpaddr0 = 0x200009ff`, which decodes as NAPOT covering
  exactly `[0x80002000, 0x80003000)` (4 KiB, the `scratch` buffer, which
  is 4 KiB aligned by construction).
- `pmpcfg0 = 0x98`: entry 0 byte = L=1, A=NAPOT (0x18), R=W=X=0.
  Entries 1-7 are A=OFF (no match).
- The L (lock) bit is required because this test runs in M-mode, and
  unlocked PMP entries are not checked against M-mode accesses (RISC-V
  privileged spec, PMP section). Locking was verified, not assumed: a
  `csrw pmpcfg0, 0` after programming reads back `0x98`, proving the
  lock holds.

## Controls (what the faults are compared against)

1. Before programming the PMP entry, the scratch buffer is written with
   `0xA5..0xA8` and read back: proves the address is good RAM, so the
   later faults come from the PMP check and not from a bad address.
2. After programming, a second buffer (`probe_outside`, at
   `0x80001038`, outside the region) is written and read back: proves
   the entry is narrow and the rest of the address space still works.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

| test | seen | mcause | mepc | mtval | resume-4 |
|---|---|---|---|---|---|
| load | 1 | 0x5 | 0x80000416 | 0x80002000 | 0x80000416 |
| store | 1 | 0x7 | 0x800004be | 0x80002000 | 0x800004be |

Identical across all three runs. Every program-internal check passed;
all three runs print `RESULT: PASS`.

What each column means:

- `mcause` 5 = load access fault, 7 = store/AMO access fault: the exact
  codes the privileged spec's trap table assigns. Ground truth is the
  spec table, and the observed values match it.
- `mepc` equals the address of the faulting instruction. This is
  checked, not eyeballed: each test's asm block lays out `auipc` (4
  bytes, never compressed), then the `lw`/`sw` (4 bytes; t1/t2 are not
  compressible registers), then the resume label. The trap handler
  resumes at the label, so the faulting instruction is always at
  `resume - 4`. The disassembly was inspected to confirm the layout
  (load: auipc at 0x80000412, lw at 0x80000416, resume at 0x8000041a;
  store: auipc at 0x800004ba, sw at 0x800004be, resume at 0x800004c2).
- `mtval` = 0x80002000 = the faulting address (start of the scratch
  buffer), matching the spec's rule that mtval carries the faulting
  address for access faults.

## Two defects found and fixed during development

Both are recorded here because they changed what was verified.

1. Trap-handler mscratch bug (mine). The first version ended with
   `csrr t0, mscratch`, which restores t0 but leaves mscratch holding
   the old t0 instead of `&pmp_save`. The second trap then used a
   garbage save area. Fixed by ending with `csrrw t0, mscratch, t0`,
   which restores t0 and reinstalls `&pmp_save` in one swap. Caught by
   QEMU `-d int` tracing showing a fetch-fault loop at pc=0.

2. Toolchain quirk (not mine): the distro `riscv64-unknown-elf-gcc`
   13.2.0 miscompiles C `&&label` (labels-as-values) at -O2, producing a
   wrong resume address (verified with a minimal reproducer: at -O0 the
   address is correct, at -O2 it points at the function entry / the
   auipc itself). Workaround: the resume address is taken inside the
   asm block with `la t0, 1f` against a numeric local label, which the
   assembler resolves exactly. No `&&label` remains in the module.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's PMP model on the `virt` machine, not real
  silicon. The trap codes 5 and 7 are architectural (spec table), but
  the check that a locked entry denies M-mode is only as good as QEMU's
  implementation of that rule.
- Only entry 0, only NAPOT, only a 4 KiB region, only hart 0, only
  M-mode. Instruction-fetch denial (X=0) and S-mode behavior are not
  tested; the module is deliberately that small.
- mepc/mtval addresses are specific to this binary's layout; the
  invariant that transfers is `mepc == resume - 4` and
  `mtval == faulting address`, re-checked by the program on every run.

## Reproduction

```
make pmp.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel pmp.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; QEMU is terminated by `timeout` afterwards
because the bare-metal image never exits QEMU on its own).
