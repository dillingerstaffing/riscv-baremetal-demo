<!-- PROOF-HEADER
Checks: 15
Mismatches: 0
Checksum: 0x9b52bcfd1c7d14c3
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: FP with mstatus.FS=Off traps illegal-instruction (backlog item "riscv sstatus-fs-off-fp-trap")

Backlog item "riscv sstatus-fs-off-fp-trap": with mstatus.FS=Off,
an FP instruction must trap as illegal-instruction (mcause 0x2);
with FS=Initial the same instruction must execute and produce
the right result.

## What was built

`src/sstatus-fs-off-fp-trap/`, a bare-metal M-mode binary sharing
only `src/boot.S` and the UART driver with the other demos. It
walks this sequence and checks every step in code:

1. Install the M-mode trap vector (sfo_trap.S), park mscratch on a
   4-word record (mcause, mepc, mtval, trap count), clear
   mstatus.MIE, require mie == 0.
2. Read and print the boot mstatus; require FS == 0 (Off).
3. Clear mstatus.FS to Off with csrc, read back and print to
   prove FS == Off.
4. Execute `fadd.d f0, f0, f0` (emitted under `.option arch,+d`
   since the module builds with `-march=rv64imac_zicsr`). The
   expected fault pc is taken from the instruction's own numeric
   local label (`la %0, 1f` in the same inline asm block), not a
   C labels-as-values address. The handler records
   mcause/mepc/mtval, counts the trap, advances mepc by 4 past
   the 4-byte fadd.d, and mrets.
5. Assert: exactly 1 trap, mcause == 0x2, mepc == the recorded
   fault-site address, FS still Off after the trap.
6. Positive control: set FS=Initial, load f1=1.5 and f2=2.25
   through `fmv.d.x` from integer bit patterns, execute
   `fadd.d f0, f1, f2`, move the f0 bits out with `fmv.x.d`,
   and assert no new trap fired and f0 == 0x400e000000000000
   (the exact double 3.75).
7. Restore the boot mstatus word exactly with csrw and confirm
   the readback.

Exit/fail code: on PASS the module writes the virt test-device
finisher word 0x5555 at 0x100000, which shuts the machine down
and QEMU exits 0. On FAIL it parks the hart in a wfi loop
without touching the finisher; the bench harness runs QEMU
under `timeout`, so a FAIL is observable as the timeout exit
status (124) in addition to the RESULT: FAIL line.

## Checks (all computed, none eyeballed)

| # | Check | Result |
|---|-------|--------|
| 1 | mtvec took the handler address | pass |
| 2 | mtvec in direct mode | pass |
| 3 | mie == 0 at boot | pass |
| 4 | misa carries the F and D bits (fadd.d needs D) | pass |
| 5 | boot mstatus FS field == 0 (Off) | pass |
| 6 | FS reads back Off after csrc | pass |
| 7 | fault phase produced exactly one trap | pass |
| 8 | trap mcause == 0x2 (illegal instruction) | pass |
| 9 | trap mepc == recorded fault-site address | pass |
| 10 | FS still Off after the trap | pass |
| 11 | positive control added no new trap (count still 1) | pass |
| 12 | f0 bits == 0x400e000000000000 (3.75) | pass |
| 13 | FS moved to Dirty after the control FP writes | pass |
| 14 | csrw restored the exact boot mstatus word | pass |
| 15 | final trap count == 1 (no extra trap anywhere) | pass |

## Measured values (3 runs, byte-identical)

- boot mstatus: 0xa00000000, FS=0
- FS-Off readback: 0xa00000000, FS=0
- fault site: 0x800003a6
- trap: mcause=0x2, mepc=0x800003a6, mtval=0x2007053
- mtval decodes to 0x02007053, the exact encoding of
  `fadd.d f0, f0, f0` (funct7=0000001, rs2=0, rs1=0, rm=111,
  rd=0, opcode=1010011), confirmed against objdump of the
  linked ELF: the fault-site label resolves to the fadd.d at
  0x800003a6 and mepc equals it.
- after trap: FS=0
- control: traps=1, f0bits=0x400e000000000000, FS=3
- restored mstatus: 0xa00000000 (== boot word)
- checks=15 mismatches=0
- checksum (FNV-1a over the measurement words: boot mstatus,
  FS-Off readback, trap mcause/mepc/mtval, fault site, trap
  count, f0 bits, control FS, restored mstatus):
  0x9b52bcfd1c7d14c3
- RESULT: PASS, QEMU exit code 0, on all 3 runs.

## Limits of verification

- Emulator, not silicon: every number above is a property of
  QEMU 8.2.2's `virt` machine, not of a physical RISC-V hart.
  The FS=Off illegal-instruction gate is implemented in QEMU's
  translator; on real hardware the same behavior is required by
  the privileged spec, but this module does not measure
  hardware.
- mtval is printed but not asserted: the privileged spec
  permits mtval to be zero or the faulting instruction bits on
  an illegal-instruction exception. This QEMU build writes the
  faulting encoding (0x2007053); the module records it in the
  checksum but does not require that value.
- The `fadd.d f0, f0, f0` fault probe and the `fadd.d f0, f1,
  f2` control cover one FP opcode each; the gate applies to all
  FP instructions, but only fadd.d was executed.

## Build log

```
$ riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/boot.S -o src/boot_sfo.o
$ riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/uart.c -o src/uart_sfo.o
$ riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sstatus-fs-off-fp-trap/sfo_main.c -o src/sstatus-fs-off-fp-trap/sfo_main.o
$ riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sstatus-fs-off-fp-trap/sfo_trap.S -o src/sstatus-fs-off-fp-trap/sfo_trap.o
$ riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o sstatus-fs-off-fp-trap.elf src/boot_sfo.o src/uart_sfo.o src/sstatus-fs-off-fp-trap/sfo_main.o src/sstatus-fs-off-fp-trap/sfo_trap.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/../lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: sstatus-fs-off-fp-trap.elf has a LOAD segment with RWX permissions
built sstatus-fs-off-fp-trap.elf
```

The RWX warning comes from the shared `link.ld` (used by every
module in this repo) and is present in the other modules' build
logs too; no new warning was introduced by this module.

Toolchain: `~/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc`
(13.2.0), first on PATH. Build run from the repo root; link
order is boot.o first so _start lands at 0x80000000.

## Run logs (QEMU 8.2.2, all 3 runs)

Run command (each run):
```
LD_LIBRARY_PATH=~/workspace/qemu/usr/lib/x86_64-linux-gnu:~/workspace/qemu/lib/x86_64-linux-gnu \
~/workspace/qemu/usr/bin/qemu-system-riscv64 -machine virt -nographic -bios none -kernel sstatus-fs-off-fp-trap.elf
```

Run 1 (run1.log):
```
sstatus-fs-off-fp-trap: FP with mstatus.FS=Off must trap illegal-instruction
setup: mtvec=0x800006b0 mie=0x0
setup: misa=0x80000000001411ad
boot: mstatus=0xa00000000 FS=0
fs-off: mstatus=0xa00000000 FS=0 (expect 0)
fault: site=0x800003a6 traps=1 mcause=0x2 mepc=0x800003a6 mtval=0x2007053
after trap: FS=0 (expect 0)
control: traps=1 (expect still 1) f0bits=0x400e000000000000 (expect 0x400e000000000000) FS=3 (expect 3)
restore: mstatus=0xa00000000 (expect boot word)
checks=15 mismatches=0
checksum=0x9b52bcfd1c7d14c3
RESULT: PASS
```

Runs 2 and 3 (run2.log, run3.log) are byte-identical to run 1
(sha256 a4322863ebb8e07c41fe29eb0502f28a936239b72f20b94cab780c90136bc86a
for all three). QEMU exit code 0 on all 3 runs (finisher
shutdown path).
