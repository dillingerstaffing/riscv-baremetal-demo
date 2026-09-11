<!-- PROOF-HEADER
Checks: 22
Mismatches: 0
Checksum: n/a
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: stvec vectored mode, exceptions at BASE, interrupts at BASE+4*cause

## Premise correction (read first)

The backlog item "riscv stvec-vectored-mode" as written claimed that
in vectored mode an illegal instruction lands at BASE+8 and an ecall
at BASE+4*cause, i.e. that synchronous exceptions vector by cause
just like interrupts. That premise is wrong.

The privileged spec's Trap Vector Base Address Register section
states the actual rule: in vectored mode, ALL synchronous exceptions
enter the handler at BASE, and only INTERRUPTS enter at BASE +
4*cause. Implementing the written premise would have meant building a
module that "proved" a behavior the hardware does not have, which
would be a shipped lie on the portfolio.

This module implements and proves the true rule instead:
- Phase A: a delegated illegal-instruction exception enters at BASE
  (slot 0), with scause = 0x2 and sepc at the illegal word.
- Phase B: a delegated supervisor software interrupt enters at
  BASE+4 (slot 1), with scause = 0x8000000000000001 and sepc at the
  interrupted instruction.

The entry address of every trap is measured, not assumed: each
vector-table stub is `jal t0, stvm_s_entry`, so the common recorder
recovers the exact hardware entry pc as t0 - 4 and stores it in the
trap log.

## What was built

`src/stvec-vectored-mode/`: a bare-metal RISC-V program (four files,
sharing only `src/boot.S` and `src/uart.c` with the other demos) that
runs in S-mode under M-mode supervision on the QEMU virt board.

- `stvm_trap.S`: a 4-byte-aligned two-slot vector table (one 4-byte
  jal stub per slot), the common S-mode trap recorder, and an M-mode
  recorder that only records (no M-mode trap is expected).
- `stvm_main.c`: M-mode setup and delegation readbacks, the
  vectored stvec install with mode/BASE readback, the S-mode drop,
  phase A (illegal word), phase B (pended SSIP), the no-pending
  control, the quiet window, and 22 checks.

## Measurements (QEMU 8.2.2, three runs, program output byte-identical)

Setup readbacks:
- stvec: written 0x800001c9, readback 0x800001c9, so MODE=1
  (vectored) and BASE = 0x800001c8 = the vector table address.
- medeleg: zero-write readback 0x0, selective write readback 0x4
  (exactly bit 2, illegal instruction).
- mideleg: zero-write readback 0x1444 (the hart's forced set),
  selective write readback 0x1446 (forced set plus exactly bit 1,
  supervisor software interrupt; bit 9 clear).

Phase A (synchronous exception):
- Exactly 1 S-mode trap, entry pc 0x800001c8 == BASE (slot 0),
  scause 0x2, sepc 0x8000034a == the illegal-word site address,
  sip at entry 0x0, 0 M-mode traps. The handler advanced sepc past
  the 4-byte word and execution continued.

Phase B (interrupt):
- sip readback after pending SSIP: 0x2. Exactly one further S-mode
  trap (2 total), entry pc 0x800001cc == BASE+4 (slot 1),
  scause 0x8000000000000001, sepc 0x80000480 == the interrupted nop,
  sip at entry 0x2 (SSIP), sip after the handler 0x0 (cleared, so the
  level-triggered source fired once), unexpected-exception marker 0x0,
  0 M-mode traps.

Controls:
- SIE on with nothing pending: 0 traps of either kind.
- Quiet window (SIE on, nothing pending): trap counts unchanged
  (S-mode 2, M-mode 0).

22 checks, 0 mismatches, verdict PASS on all three runs. Program
output is byte-identical across the three runs (verified by sha256
of the log bodies; the only differing bytes are the QEMU timeout
pid in the final stderr line, which is not program output).

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/stvec-vectored-mode/stvm_trap.S -o src/stvec-vectored-mode/stvm_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/stvec-vectored-mode/stvm_main.c -o src/stvec-vectored-mode/stvm_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o stvec-vectored-mode.elf src/boot.o src/uart.o src/stvec-vectored-mode/stvm_trap.o src/stvec-vectored-mode/stvm_main.o
```

Toolchain: riscv64-unknown-elf-gcc 13.2.0
(~/workspace/toolchains/ubuntu-rv64). QEMU: qemu-system-riscv64 8.2.2
(~/workspace/qemu). The build emits the usual "LOAD segment with RWX
permissions" ld note, also seen on every other module in this repo.

## Run output (one of three identical runs)

```
stvec-vectored-mode: exception-at-BASE vs interrupt-at-BASE+4*cause test
boot: medeleg=0x0 mideleg=0x1444
medeleg: write=0x0 readback=0x0 write=0x4 readback=0x4
mideleg: write=0x0 readback=0x1444
mideleg: write readback=0x1446
stvec: written=0x800001c9 readback=0x800001c9
control: s_traps=0 m_traps=0
exc: spins=0 s_traps=1 entry=0x800001c8 base=0x800001c8 scause=0x2 sepc=0x8000034a site=0x8000034a sip-at-entry=0x0
int: sip-after-pend=0x2
int: spins=0 s_traps=2 entry=0x800001cc base+4=0x800001cc scause=0x8000000000000001 sepc=0x80000480 expected=0x80000480 sip-at-entry=0x2
int: sip-after-handler=0x0 marker=0x0 m_traps=0
quiet: s_traps=2 m_traps=0
RESULT: PASS
done
```

## What was NOT verified

- Vector slots beyond slot 0 (exceptions) and slot 1 (SSI) were not
  exercised; the sibling module `stvec-vectored` covers interrupt
  slots 5 (timer) and 9 (external).
- medeleg bit 2 delegation was exercised only for the illegal
  instruction; breakpoint (bit 3) and other exception codes were not
  delegated or tested.
- Single hart only; nothing about this module tests multi-hart trap
  behavior.
