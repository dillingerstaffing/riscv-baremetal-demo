<!-- PROOF-HEADER -->
Checks: 18
Mismatches: 0
Checksum: 0x802872738f9db42a
Environment: QEMU 8.2.2
Verdict: PASS

# Proof: sret executed in U-mode raises illegal-instruction (backlog item: riscv sret-to-umode-fault)

## What was built

`src/sret-to-umode-fault/`: a bare-metal RISC-V program that checks
the one hardware behavior under test: sret is a privileged
instruction, executable only in S-mode or M-mode, so executing it in
U-mode must trap to M-mode with mcause=2 (illegal instruction), mepc
at the sret, and mstatus.MPP=0 (the trap hardware records the
pre-trap privilege). The privileged specification (Volume II,
section on supervisor instructions) defines SRET as raising an
illegal-instruction exception when executed in U-mode.

M-mode boots, writes medeleg=0 and mie=0 and reads both back (every
trap must stay in M-mode, no interrupt may fire), installs a
direct-mode mtvec and a counting direct-mode stvec (control: must
see 0 traps), opens the whole address space to S/U-mode with one
PMP NAPOT entry R/W/X, then runs two phases:

- Phase 1, the control: sret from M-mode with SPP=1 and sepc at the
  S-mode landing pad (sret is legal in M-mode). The S-mode pad
  flags arrival, records the address of its own sret, points sepc
  past it, keeps SPP=1, and executes sret. The control claim is that
  this sret returns to S-mode with no trap. The pad then flags the
  return and issues the one scripted ecall back to M-mode (medeleg
  bit 9 is clear, so the ecall traps in M-mode with mcause 0x9).
- Phase 2: the ecall handler records ecall_cause/ecall_epc, sets
  mstatus.MPP=M-mode, points mepc at phase2_drop, and mrets. The
  phase-2 driver points sepc at the U-mode landing pad, clears
  sstatus.SPP, and srets (legal in M-mode), dropping to U-mode. The
  U-mode pad records the address of its sret and executes it.
  U-mode may not execute sret, so the hart traps to M-mode with
  mcause=2; the handler records mcause/mepc/mtval/mstatus, bumps
  the trap counter, and jumps to a C continuation. The script ends
  at the trap; no mret.

The continuation prints the recorded values, a checksum over them,
runs 18 checks, and on PASS writes the finisher word 0x5555 at
0x100000 for a QEMU exit code of 0; on FAIL it parks the hart in a
wfi loop. Four files (`stu_main.c`, `stu_trap.S`, `PROOF.md`,
`bench-logs/`), sharing only `src/boot.S` and `src/uart.c` with the
other demos. All trap-site addresses come from in-asm numeric local
labels (`la t1, 1f`), never from C computed-goto labels.

## Configuration under test

QEMU 8.2.2 `virt` board, single hart, `-bios none`, ELF loaded with
`-kernel` (execution starts at the 0x80000000 load address;
`src/boot.S` is first in the link order). medeleg=0, mie=0,
mstatus.MIE=0 throughout, so the two scripted synchronous traps are
the only traps that can fire.

## Sequence and controls

1. medeleg read back 0 after writing 0: the illegal-instruction
   trap cannot be delegated away from M-mode.
2. mie read back 0, mstatus.MIE read back clear: no interrupt path
   is armed.
3. stvec installed in direct mode; its handler counts S-mode traps
   and parks. Final S-mode trap count is 0.
4. One PMP NAPOT entry opens the whole address space R/W/X to
   S/U-mode (without it, lower-privilege fetches fault before the
   pads run).
5. Control: the S-mode sret must execute and return to S-mode with
   no trap (`control_ok=1`, `control_after=1`), and the phase-1
   return must be exactly one S-mode ecall (`ecall_cause=0x9`,
   `ecall_epc` at the recorded ecall site).
6. Core: the U-mode sret must trap to M-mode with mcause=2, mepc at
   the recorded sret site, trapped mstatus.MPP=0, and mtval holding
   the faulting instruction word.

## Measured results

3-run table (byte-identical on every run, diff of the three run
logs is empty):

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| `medeleg` readback after writing 0 | `0x0` | identical | identical |
| `mie` readback after writing 0 | `0x0` | identical | identical |
| mstatus.MIE after clear | `0` | identical | identical |
| control: `control_ok` | `1` | identical | identical |
| control: `control_after` (S-mode sret returned) | `1` | identical | identical |
| control: `ecall_cause` | `0x9` | identical | identical |
| control: `ecall_epc` vs recorded site | `0x80000304` = `0x80000304` | identical | identical |
| M-mode trap count | `2` | identical | identical |
| `mcause` | `0x2` (illegal instruction) | identical | identical |
| `mepc` vs recorded sret site | `0x80000344` = `0x80000344` | identical | identical |
| trapped `mstatus.MPP` | `0` (U-mode) | identical | identical |
| `mtval` | `0x10200073` (the trapped `sret` encoding) | identical | identical |
| S-mode traps | `0` | identical | identical |
| checksum over the measured values | `0x802872738f9db42a` | identical | identical |
| checks passed / failed | 18 / 0 | identical | identical |
| QEMU exit code | 0 | 0 | 0 |
| verdict | PASS | PASS | PASS |

`mcause` read 2, `mepc` matched the recorded sret site
`0x80000344`, and the trapped `mstatus.MPP` read 0 in all three
runs; the finisher word shut the machine down and QEMU exited 0
each time.

Note: `mtval` is not 0 on this QEMU build. QEMU 8.2.2 writes the
faulting instruction word into `mtval` on an illegal-instruction
trap; `0x10200073` decodes as `sret`, exactly the trapping
instruction. The check compares `mtval` against the actual word at
`mepc` rather than a guessed constant.

## Raw QEMU output

### Run 1 (bench-logs/run1.log)

```
sret-to-umode-fault: sret in U-mode traps illegal-instruction
deleg: medeleg=0x0
phase1: control_ok=1 control_after=1 ecall_cause=0x9 ecall_epc=0x80000304 ecall_site=0x80000304
phase2: count=2 mcause=0x2 mepc=0x80000344 mtval=0x10200073 mpp=0 expected=0x80000344
phase2: s_traps=0 cksum=0x802872738f9db42a
Checks: 18
Mismatches: 0
Checksum: 0x802872738f9db42a
Environment: QEMU 8.2.2
Verdict: PASS
```

### Run 2 (bench-logs/run2.log)

Byte-identical to run 1 (empty diff).

### Run 3 (bench-logs/run3.log)

Byte-identical to run 1 (empty diff).

## Build log (bench-logs/build.log)

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/boot.S -o src/boot.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/uart.c -o src/uart.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sret-to-umode-fault/stu_trap.S -o src/sret-to-umode-fault/stu_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sret-to-umode-fault/stu_main.c -o src/sret-to-umode-fault/stu_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o sret-to-umode-fault.elf src/boot.o src/uart.o src/sret-to-umode-fault/stu_trap.o src/sret-to-umode-fault/stu_main.o
```

Toolchain: Ubuntu gcc-riscv64-unknown-elf 13.2.0.

## Limits

- Emulator, not silicon: the sret privilege check measured is QEMU
  8.2.2's model of the `virt` board. The privileged specification
  defines SRET as raising an illegal-instruction exception when
  executed in U-mode, and the measured mcause/mepc/MPP/mtval values
  match that definition exactly.
- Single hart, no interrupts armed: the module proves the
  synchronous trap rule, not sret behavior under concurrent trap
  sources.
