# Proof: mstatus.MPP encoding write/return/verify

## What was built

`src/mpp-encoding/`: a bare-metal RISC-V program that isolates one
mechanism, the mstatus.MPP field (bits 12:11) and the mret privilege
transition. For each of the four MPP encodings it writes the
encoding to the field, reads back what the CSR actually holds,
executes mret with mepc at a one-instruction ecall payload, and
lets the payload's ecall trap back to the M-mode handler. The
handler records mcause, mepc, mtval, and the mstatus value seen on
trap entry, then returns to M-mode at phase_done, which prints the
write/return/verify tuple and runs the checks before the next
phase.

Three files, about 350 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos.

- `mpp_main.c`: UART bring-up, trap vector installation, PMP
  setup (one NAPOT R/W/X entry covering the whole address space,
  needed because unprogrammed PMP denies lower modes all access),
  the MPP write/readback, the mret drop block, the C trap
  dispatcher, and the M-mode reporter with the PASS/FAIL checks.
  The payload address is taken with an in-asm numeric local label
  (`la t0, 1f` / `1:`) and stored to the global with an explicit
  `sd` before the mret, since the block never falls through (an
  output operand the compiler would store after the template would
  never execute). The `.option norvc` keeps the ecall a 4-byte
  instruction so mepc is exact.
- `mpp_trap.S`: minimal M-mode trap entry. mscratch points at the
  8-word `mpp_regs` array; on entry it swaps t0, records
  mcause/mepc/mtval/mstatus, calls the C dispatcher for the resume
  pc, writes it to mepc, sets mstatus.MPP to M-mode, restores t0/t1,
  and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make mpp-encoding.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel mpp-encoding.elf`
(or `make run-mpp-encoding`).

## Configuration under test

- Hart: mhartid = 0, single hart, boots in M-mode (QEMU boots the
  ELF straight into M-mode with `-bios none`).
- PMP: one NAPOT entry covering the whole address space, R/W/X,
  unlocked, programmed before the first drop.
- Payload: `mret` followed by one 4-byte `ecall` at 0x800002d2.
  The program checks `mepc == payload_addr` exactly, so a layout
  mistake would show up as a FAIL, not a silent wrong number.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

Byte-identical across all three runs; QEMU exited 0 on every run
(the PASS path shuts the machine down through the virt
test-device finisher).

| phase | wrote MPP | readback MPP | landing addr | mcause | mepc | mtval | entry MPP |
|---|---|---|---|---|---|---|---|
| 0 | 0 (U) | 0 | 0x800002d2 | 0x8 | 0x800002d2 | 0x0 | 0 |
| 1 | 1 (S) | 1 | 0x800002d2 | 0x9 | 0x800002d2 | 0x0 | 1 |
| 2 | 2 (reserved) | 0 | 0x800002d2 | 0x8 | 0x800002d2 | 0x0 | 0 |
| 3 | 3 (M) | 3 | 0x800002d2 | 0xb | 0x800002d2 | 0x0 | 3 |

RESULT: PASS on all three runs. Exactly one trap fired per phase.

What each value means:

- The mcause code is the independent evidence of the landing
  mode: the hardware derives it from the privilege mode at trap
  time. Had any mret failed to drop privilege, the same ecall
  would have raised 0xb (M-mode ecall) instead of 0x8 or 0x9.
- The trap-entry MPP bits are a second, independent record of
  the mode the trap came from: 0, 1, and 3 match the written
  encodings exactly.
- mepc 0x800002d2 is exactly the payload's ecall instruction,
  matching the address the drop block recorded before each
  transition; the invariant is re-checked by the program on every
  phase.
- mtval 0x0 is what the spec requires on ecall traps.
- Phase 2 (reserved encoding 10) is the measured emulator
  behavior: the MPP write of 2 does not stick in the CSR, the
  readback is 0, and the subsequent mret behaves as a drop to
  U-mode (mcause 8, entry MPP 0). The write is coerced at write
  time; there is no way to make this emulator mret into the
  reserved encoding. This was found by probing before the module
  was written (write MPP=0..3, read back: 0, 1, 0, 3) and the
  phase's checks record it rather than assuming it.

## Build log (real, from `bench-logs/build.log`)

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mpp-encoding/mpp_trap.S -o src/mpp-encoding/mpp_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mpp-encoding/mpp_main.c -o src/mpp-encoding/mpp_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o mpp-encoding.elf src/boot.o src/uart.o src/mpp-encoding/mpp_trap.o src/mpp-encoding/mpp_main.o
ld: warning: mpp-encoding.elf has a LOAD segment with RWX permissions
```

(The RWX warning is the linker's standard remark for these
flat bare-metal images; every module in this repo links the same
way.)

## Full run output (run 1; runs 2 and 3 are byte-identical)

```
mstatus.MPP encoding write/return/verify

phase 0: MPP=00 (U), write done, readback MPP=0

--- phase 0: wrote MPP=00 (U) ---
write/return: wrote MPP=0, MPP readback=0
landing (payload ecall) address: 0x800002d2
trap: mcause=0x8 mepc=0x800002d2 mtval=0x0 entry MPP=0

phase 1: MPP=01 (S), write done, readback MPP=1

--- phase 1: wrote MPP=01 (S) ---
write/return: wrote MPP=1, MPP readback=1
landing (payload ecall) address: 0x800002d2
trap: mcause=0x9 mepc=0x800002d2 mtval=0x0 entry MPP=1

phase 2: MPP=10 (reserved), write done, readback MPP=0

--- phase 2: wrote MPP=10 (reserved) ---
write/return: wrote MPP=2, MPP readback=0
landing (payload ecall) address: 0x800002d2
trap: mcause=0x8 mepc=0x800002d2 mtval=0x0 entry MPP=0

phase 3: MPP=11 (M), write done, readback MPP=3

--- phase 3: wrote MPP=11 (M) ---
write/return: wrote MPP=3, MPP readback=3
landing (payload ecall) address: 0x800002d2
trap: mcause=0xb mepc=0x800002d2 mtval=0x0 entry MPP=3

RESULT: PASS
```

## Limits of verification (read before citing numbers)

- This measures QEMU 8.2.2's `virt` machine, an emulator, not
  silicon. The mcause encodings (8 = ecall from U-mode, 9 = from
  S-mode, 11 = from M-mode) are mandated by the privileged spec,
  so the cause codes transfer to silicon; what is
  emulator-specific is everything around them (exact addresses,
  the trap-entry mstatus value) and, in particular, the reserved
  encoding behavior: the write-time coercion of MPP=2 to 0 is
  QEMU's choice, and real hardware is only required to treat 10
  as reserved.
- One hart, four drops, one payload instruction each, no
  interrupts enabled during the drops. Nested traps and lower
  modes doing real work before the ecall are not tested; the
  module is deliberately that small.
- Addresses are specific to this binary's layout; the invariant
  that transfers is mepc == the recorded payload address,
  re-checked by the program on every phase.

## Reproduction

```
make mpp-encoding.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mpp-encoding.elf
```

Toolchain used: xPack riscv64-unknown-elf-gcc 15.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `RESULT: PASS`; QEMU exits 0 by itself because the
PASS path shuts the machine down through the virt test-device
finisher).
