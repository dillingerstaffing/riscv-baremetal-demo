# Proof: misa readback with per-extension instruction probes

## What was built

`src/csr/`: a bare-metal RISC-V program that treats the hart's `misa`
CSR as the ground-truth declaration of implemented ISA extensions and
checks each claimed extension by executing one hand-written
instruction characteristic of it under an M-mode trap handler that
records mcause/mepc/mtval on any trap. Four files, about 330 lines
total, sharing only `src/boot.S` and `src/uart.c` with the other demos.

- `csr_main.c`: UART bring-up, trap vector installation, `misa` /
  `marchid` / `mimpid` readback (each read twice, the two reads must
  agree), MXL and extension-bitmap decode, one probe per reported
  extension letter, and the PASS/FAIL verdict.
- `csr_trap.S`: minimal M-mode trap entry. mscratch points at the
  7-word `csr_save` array; on entry it swaps t0, records
  mcause/mepc/mtval, loads mepc from the resume address the probe
  stored, flags the trap seen, restores t0/t1, and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make csr.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel csr.elf`
(or `make run-csr`).

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- QEMU 8.2.2 `virt` machine.
- `misa` read: `0x80000000001411ad`, so MXL = 2 (RV64) and the bitmap is
  `0x1411ad`, i.e. letters `ACDFHIMSU`.
- `marchid` read: `0x0` (stable across both reads).
- `mimpid` read: `0x0` (stable across both reads).
- The values are published as read. No expectation about marchid/mimpid
  is hard-coded, since they vary across QEMU versions.

## Probes (one per reported letter)

Each probe is a single inline-asm block. Before the probe instruction
the block stores the resume address (taken with an in-asm numeric
local label, `la t0, 1f`) into `csr_save[5]` and clears the seen flag
in `csr_save[6]`, so any trap lands in the handler and resumes at
label `1`. A probe passes only if no trap fired AND the result is the
exact expected value. Probes that need arch extensions beyond the
`-march=rv64imac_zicsr` build flags enable them locally with
`.option arch, +f` / `+d` / `+h` inside their own asm block.

| letter | probe instruction | encoding in csr.elf | result check |
|---|---|---|---|
| I | `add` (100 + 23) | 4-byte `add` | result == 123 |
| M | `mul` (6 * 7) | `02730733 mul a4,t1,t2` | result == 42 |
| A | `amoswap.w` on aligned RAM word | `0873262f amoswap.w a2,t2,(t1)` | rd == old contents (0x12345678), memory == 0xdeadbeef (read back with `lwu`) |
| C | `c.addi t1, 5` | 2-byte `0315` (C.ADDI; bits[1:0]=01 is never a valid 32-bit opcode, so a hart without C would trap) | result == 5 |
| F | `fadd.s` (1.0f + 2.0f) | `00b57653 fadd.s fa2,fa0,fa1` | bits == 0x40400000 (3.0f), bit-exact |
| D | `fadd.d` (1.0 + 2.0) | `02b57653 fadd.d fa2,fa0,fa1` | bits == 0x4008000000000000 (3.0), bit-exact |
| H | `hfence.gvma zero, zero` | `62000073 hfence.gvma` | no trap (legal no-op in M-mode on a hart with H) |
| S | (none) | | privilege mode, not an instruction; reported only |
| U | (none) | | privilege mode, not an instruction; reported only |

The F and D probes need `mstatus.FS` set (otherwise FP instructions
trap as illegal even on a hart with F/D); the program sets FS to Dirty
with `csrs mstatus, 0x6000` and checks it stuck before probing. The
1.0/2.0/3.0 bit patterns are loaded with `fmv.w.x` / `fmv.d.x` and
read back with `fmv.x.w` / `fmv.x.d`, so the F/D checks compare exact
bit patterns, not approximate floats.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

```
misa    read1=0x80000000001411ad read2=0x80000000001411ad
marchid read1=0x0 read2=0x0
mimpid  read1=0x0 read2=0x0
misa MXL=2 (2 = RV64) bitmap=0x1411ad
misa extensions reported: ACDFHIMSU
probe A (amoswap.w): seen=0 PASS
probe C (c.addi): seen=0 PASS
probe D (fadd.d): seen=0 PASS
probe F (fadd.s): seen=0 PASS
probe H (hfence.gvma): seen=0 PASS
probe I (add): seen=0 PASS
probe M (mul): seen=0 PASS
letter S: privilege mode, no instruction probe
letter U: privilege mode, no instruction probe
RESULT: PASS
```

Identical across all three runs (the only log difference is the pid in
the timeout kill line). Every program-internal check passed: both
reads of each identity CSR agreed, MXL == 2, `mstatus.FS` stuck at
Dirty, and all seven probes executed without a trap and produced the
exact expected results.

## One defect found and fixed during development

The A probe's memory readback check initially used `lw` and compared
against `0xdeadbeef`. The probe itself was correct (no trap, rd held
the old contents); the check was wrong because `lw` sign-extends, so
the register held `0xffffffffdeadbeef`. Fixed by reading back with
`lwu`. Caught by the program's own check printing FAIL on the first
run, before any code was committed.

## Limits of verification (read before citing numbers)

- This measures QEMU 8.2.2's `virt` machine, not silicon. `misa`,
  `marchid`, and `mimpid` are emulator-provided values; the probes
  confirm the emulated hart decodes and executes one instruction per
  claimed extension, not that a physical core would behave the same.
- One instruction per extension is a spot check, not an ISA
  conformance suite. It confirms the extension's instructions decode
  and execute, not every instruction or corner case in the extension.
- S and U are reported from the bitmap but not exercised; they denote
  privilege modes, and S-mode entry is covered by the `smode` module.
- Only hart 0, only M-mode. A letter the program has no probe for
  would print "reported by misa, no probe defined" and would not fail
  the run; on this QEMU every reported letter has a probe or is a
  privilege mode.

## Reproduction

```
make csr.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel csr.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; QEMU is terminated by `timeout` afterwards
because the bare-metal image never exits QEMU on its own).
