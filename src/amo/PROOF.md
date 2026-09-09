# Proof: misaligned LR/SC experiment

## What was built

`src/amo/`: a bare-metal RISC-V program that installs an M-mode trap
handler recording mcause/mepc/mtval on any trap, then issues LR/SC
sequences at fixed addresses derived from an aligned 64-byte scratch
buffer:

- T1: `lr.w` at base+2 (not 4-byte aligned).
- T2: aligned `lr.w` at base to establish the reservation, then `sc.w`
  at base+2 (not 4-byte aligned) storing 0x5A5A5A5A.
- T3: misaligned `lr.w` at base+2 followed by `sc.w` to the same
  address storing 0xA5A5A5A5. Run only if T1's lr completes; on this
  machine it does not (see results), so T3's block is compiled in but
  the pair can never get past the lr.

For each sequence the program reports whether a trap fired. On a trap
it prints the exact trap register values and checks internal
consistency (the misaligned-cause code, mtval equal to the faulting
address, mepc equal to the faulting instruction's address, which is
known exactly because each block uses `.option norvc` with a fixed
layout). On transparent completion it prints the observed values: the
loaded word for lr (checked against a byte-by-byte reconstruction of
the same memory) and the sc return value (0 = success, 1 = failure)
with the affected memory read back to confirm what the store did.

Four files, about 300 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos.

- `amo_main.c`: UART bring-up, trap vector installation, aligned-access
  control, the three test sequences, and the PASS/FAIL verdict. The
  program's checks verify its own accounting (right cause codes,
  right addresses, right round-trip bytes), never which behavior the
  machine "must" show.
- `amo_trap.S`: minimal M-mode trap entry. mscratch points at the
  8-word `amo_save` array; on entry it swaps t0, records
  mcause/mepc/mtval, loads mepc from the resume address the test
  stored, flags the trap seen, restores t0/t1, and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make amo.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel amo.elf`
(or `make run-amo`).

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- Scratch buffer: 64 bytes at `0x800011b0` (16-byte aligned).
- T1 lr address: `0x800011b2` (base+2, not 4-byte aligned).
- T2: aligned lr at `0x800011b0`, then sc at `0x800011b2` storing
  0x5A5A5A5A.
- T3 pair address: `0x800011b2` storing 0xA5A5A5A5 (not reached on
  this machine).

## Exact-layout construction

Each test's asm block is wrapped in `.option push` / `.option norvc` /
`.option pop` so every instruction in the trap window is the 4-byte
form (the assembler would otherwise fold `addi t0, reg, 0` into the
2-byte c.mv, as caught by disassembly in the misaligned module). The
disassembly confirms the layout (see `bench-logs/build.log` context
and `/tmp` notes in the raw disassembly):

- T1: auipc (A=0x800004b4), addi (A+4), `lr.w` at 0x800004bc,
  resume label at 0x800004c0. A trap on the lr gives
  mepc == resume - 4.
- T2: auipc, addi, aligned `lr.w` at 0x80000584, `addi t0, t0, 2`,
  lui/addiw (the `li` expansion for 0x5A5A5A5A), `sc.w` at
  0x80000594, resume label at 0x80000598. The sc is the last
  instruction before the label, so a trap on the sc gives
  mepc == resume - 4.
- T3: same shape as T2 with the lr at 0x80000642 misaligned and the
  `sc.w` at 0x8000064e just before the resume label at 0x80000652.

The program checks `mepc == resume - 4` whenever a trap fires at the
instruction just before the resume label, so a layout mistake would
show up as a FAIL, not as a silent wrong number.

## Control

Before the tests, the scratch buffer is written and read back with
aligned word accesses (`0xA1B2C3D4 + i`): proves the address is good
RAM, so whatever the LR/SC sequences do comes from the access itself
and not from a bad address.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

Byte-identical across all three runs (the only log difference is the
pid in the timeout kill line).

| test | trap? | mcause | mepc | mtval | observed values |
|---|---|---|---|---|---|
| T1 `lr.w` @ 0x800011b2 | yes | 4 | 0x800004bc | 0x800011b2 | mepc is exactly the `lr.w` (resume - 4); mtval is the faulting address |
| T2 aligned `lr.w` @ base, then `sc.w` @ 0x800011b2 | no | 0 | 0 | 0 | sc return = 1 (failure); memory at base+2..base+5 unchanged (b2 a1 d5 c3, the control fill) |
| T3 misaligned `lr.w` + `sc.w` pair | n/a | n/a | n/a | n/a | skipped: T1's lr traps, so the pair can never reach the sc |

What each value means:

- T1: the misaligned `lr.w` traps with mcause 4 (load address
  misaligned), mepc pointing at the faulting `lr.w`, and mtval holding
  the faulting address. The handler resumes at the label past the lr
  and the program continues. This is the measured ground truth on
  this machine: QEMU 8.2.2 raises a load address misaligned fault for
  a 2-byte-misaligned `lr.w`.
- T2: the misaligned `sc.w` does NOT trap. It completes and returns 1
  in rd, i.e. failure, and the four bytes at base+2 are unchanged
  (still the control fill B2 A1 D5 C3, verified byte by byte). So on
  this machine a misaligned sc with a live reservation from an aligned
  lr drops the reservation instead of faulting: no store happens, no
  exception is raised. Notably the sc behaves differently from the lr
  (trap) even though both are misaligned word atomics.
- T3: never executed, because the finding of T1 is that a misaligned
  lr cannot complete on this machine. The program detects this at run
  time and reports the skip instead of pretending the pair ran.

Every program-internal check passed; all three runs print
`RESULT: PASS`.

## One defect found and fixed during development

A leftover placeholder `check(...)` with a tautological condition
(`|| 1`) slipped into the T2 sc-failure path in the first draft. It
was caught on re-reading before the first run and deleted; the real
byte-by-byte unchanged-memory check remained. The shipped code
contains no tautological checks.

## Limits of verification (read before citing numbers)

- This measures the behavior of QEMU 8.2.2's `virt` machine, an
  emulator. It is emulator behavior, not silicon. The RISC-V spec
  gives implementations latitude on misaligned LR/SC (they may trap
  or be handled), so these findings document what QEMU does, not what
  the architecture mandates: on this emulator a misaligned `lr.w`
  raises mcause 4 while a misaligned `sc.w` returns 1 with no trap.
  Real cores may differ in either direction.
- Only one misaligned offset (base+2), only hart 0, only M-mode, only
  word-width atomics. The T3 pair path was compiled but not
  exercised, because T1's lr trap makes it unreachable on this
  machine. Halfword/doubleword AMOs and amo* (non-LR/SC) atomics are
  not tested; the module is deliberately that small.
- Addresses are specific to this binary's layout; the invariant that
  transfers is the mepc/resume relationship, re-checked by the program
  on every run.

## Reproduction

```
make amo.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel amo.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; QEMU is terminated by `timeout` afterwards
because the bare-metal image never exits QEMU on its own).
