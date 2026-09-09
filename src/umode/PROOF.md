# Proof: M-mode to U-mode trap transition

## What was built

`src/umode/`: a bare-metal RISC-V program that isolates one
mechanism, the M-mode to U-mode trap transition. In M-mode it
installs an mtvec trap handler recording mcause/mepc/mtval and the
mstatus value seen on trap entry, opens the address space with one
PMP NAPOT R/W/X entry, then executes an `mret` with mstatus.MPP
cleared to 0 (U-mode) into a one-instruction U-mode payload: a single
`ecall`. The payload's ecall traps back to M-mode; the handler
records the trap registers and returns to M-mode at the reporter,
which prints the exact trap values and the verdict.

Four files, about 250 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos.

- `umode_main.c`: UART bring-up, trap vector installation, PMP
  setup, the MPP-clearing `mret` drop into the U-mode payload, the
  C trap dispatcher, and the M-mode reporter with the PASS/FAIL
  checks. The payload address is taken with an in-asm numeric local
  label (`la t0, 1f` / `1:`) and stored to the global with an
  explicit `sd` before the `mret`, since the block never falls
  through (an output operand the compiler would store after the
  template would never execute; caught on the first run).
- `umode_trap.S`: minimal M-mode trap entry. mscratch points at the
  8-word `umode_regs` array; on entry it swaps t0, records
  mcause/mepc/mtval/mstatus, calls the C dispatcher for the resume
  pc, writes it to mepc, sets mstatus.MPP to M-mode, restores t0/t1,
  and returns with mret. The U-mode payload is one instruction, so
  no register state needs to survive the trap.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make umode.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel umode.elf`
(or `make run-umode`).

## Configuration under test

- Hart: mhartid = 0, single hart, boots in M-mode (QEMU boots the
  ELF straight into M-mode with `-bios none`).
- PMP: one NAPOT entry covering the whole address space, R/W/X,
  unlocked. Required because unprogrammed PMP denies lower modes
  all access; without it the first U-mode instruction fetch would
  fault.
- Payload layout (from disassembly): `mret` at 0x8000043c, the
  payload `ecall` at 0x80000440 as a 4-byte instruction (`.option
  norvc` in the drop block). The program checks `mepc ==
  payload_addr` exactly, so a layout mistake would show up as a
  FAIL, not a silent wrong number.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

Byte-identical across all three runs (the only log difference is the
pid in the timeout kill line).

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| mcause | 0x8 | 0x8 | 0x8 |
| mepc | 0x80000440 | 0x80000440 | 0x80000440 |
| mtval | 0x0 | 0x0 | 0x0 |
| mstatus at trap entry | 0xa00000000 | 0xa00000000 | 0xa00000000 |
| payload ecall address | 0x80000440 | 0x80000440 | 0x80000440 |
| traps observed | 1 | 1 | 1 |
| RESULT | PASS | PASS | PASS |

What each value means:

- mcause 8 is the privileged spec's code for "environment call from
  U-mode". The hardware derives the code from the privilege mode at
  trap time, so 8 (and not 11, the M-mode ecall code) is the
  independent evidence that the payload really executed in U-mode:
  had the `mret` failed to drop privilege, the same `ecall`
  instruction would have raised 11 instead.
- mepc 0x80000440 is exactly the payload's `ecall` instruction
  (the instruction after the `mret` at 0x8000043c), matching the
  address the drop block recorded before the transition.
- mstatus 0xa00000000 at trap entry has MPP (bits 11-12) = 0,
  U-mode: a second, independent record of the mode the trap came
  from. (Bits 33 and 35 are UXL/SXL = 64-bit, QEMU's reset values.)
- mtval 0x0 is what the spec requires on ecall traps.
- Exactly one trap fired, and the handler's `mret` with MPP = M
  resumed the reporter in M-mode, which printed the report.

## One defect found and fixed during development

The first draft took the payload address as a register output
operand (`"=r"`); the `mret` inside the template diverts control, so
the compiler's post-template store to the global never executed and
the reporter saw 0x0 (RESULT: FAIL, trap values themselves already
correct: mcause 8, mepc 0x80000440). Fixed by emitting an explicit
`sd` to a `"=m"` operand before the `mret`; all three runs after the
fix print PASS.

## Limits of verification (read before citing numbers)

- This measures QEMU 8.2.2's `virt` machine, an emulator. The
  mcause encoding itself (8 = ecall from U-mode) is mandated by the
  privileged spec, so the cause code transfers to silicon; what is
  emulator-specific is everything around it (exact addresses, the
  trap-entry mstatus value, timing).
- One hart, one transition, one payload instruction, no interrupts
  enabled during the drop. Nested traps, S-mode in the chain, and
  U-mode doing real work before the ecall are not tested; the
  module is deliberately that small.
- Addresses are specific to this binary's layout; the invariant
  that transfers is mepc == the recorded payload address,
  re-checked by the program on every run.

## Reproduction

```
make umode.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel umode.elf
```

Toolchain used: xPack riscv-none-elf-gcc 15.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `RESULT: PASS`; QEMU is terminated by `timeout`
afterwards because the bare-metal image never exits QEMU on its own).
