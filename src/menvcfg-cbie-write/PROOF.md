<!-- PROOF-HEADER
Checks: 9
Mismatches: 0
Checksum: 0x695a0fc4812f73e3
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: menvcfg.CBIE reads back exactly as written (0, 1, 2, 3, WARL legalize)

## What was built

`src/menvcfg-cbie-write/`: a bare-metal RISC-V program that verifies,
on the QEMU `virt` board, the write/readback contract of
`menvcfg.CBIE` (bits 5:4), the WARL field that governs the
cache-block-invalidate extension behavior. The experiment runs entirely in
M-mode on hart 0 (QEMU boots the ELF straight into M-mode with
`-bios none`, so no privilege drop is needed), and the module shares
only `src/boot.S` and `src/uart.c` with the other demos.

- `cbie_main.c`: prints the boot `menvcfg` value; writes each CBIE
  value 1, 2, 3 with all other bits held at their boot values and
  requires each readback to equal the written value exactly (the
  field takes every legal encoding, no other bit moved); writes
  CBIE=0 and requires the exact readback; writes all-ones twice and
  publishes both legalized readbacks, asserting only what the spec
  and this hart guarantee (the CBIE field survives the write, bits
  set at boot do not vanish, the two legalizations agree) and
  reporting the rest; restores the boot value and requires the
  exact readback; prints and requires a zero trap count. A 64-bit
  FNV-1a checksum is fed the boot value, the four CBIE
  write/readback pairs, the two legalized all-ones readbacks, and
  the restore readback in a fixed order and printed on the
  completion path; it carries no absolute addresses or live
  counters, so it is identical across runs.
- `cbie_trap.S`: minimal M-mode trap entry (direct mode). Any trap
  during this experiment is unexpected (the program never issues a
  trapping instruction), so the handler records
  `mcause`/`mepc`/`mtval` and a trap count into `cbie_save`, then
  parks the hart in a `wfi` loop. Reaching the printed verdict
  already implies zero traps, and the program also checks the
  count explicitly.
- `build.sh`, `PROOF.md` (this file), `bench-logs/` with the build
  log, three raw QEMU run logs, and `host-cpu.txt`.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 9 checks held. On PASS the machine is shut
down through the virt test-device finisher (QEMU exits 0); on FAIL
the hart parks in a `wfi` loop without touching the finisher, so a
FAIL is observable as exit status 124 under `timeout`.

Build: `build.sh` runs direct `riscv64-unknown-elf-gcc` invocations
matching the repo Makefile pattern (`-Wall -Wextra -O2
-ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic
-march=rv64imac_zicsr`), logged in `bench-logs/build.log`.
(`boot.o` links first so `_start` lands at 0x80000000, the address
QEMU's `-kernel` loader starts at.)
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel menvcfg-cbie-write.elf` under `timeout` so a parked-hart
FAIL is observable as exit status 124. QEMU is the 8.2.2 build
from `~/workspace/qemu` (system `qemu-system-riscv64` on this VM
fails with a missing `libfdt.so.1`).

## Configuration under test

- Hart: hart 0, single hart, M-mode throughout. QEMU boots the ELF
  straight into M-mode with `-bios none`.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`),
  `riscv64-unknown-elf-gcc` 13.2.0, `-march=rv64imac_zicsr`.
- Host CPU: AMD EPYC 9D25 126-Core Processor
  (`bench-logs/host-cpu.txt`).
- `mtvec` points at the park-on-trap handler; no delegation (M-mode
  keeps every trap).

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs exited 0 via the test-device finisher, and the three logs are
byte-identical (md5 `09728f04fc66a2ee129b0b177dff5720` on all three).

| step | operation | measured readback |
|---|---|---|
| boot | `csrr menvcfg` | 0x2000000000000000, CBIE(bits5:4)=0, on all 3 runs |
| cbie=1 | write 0x2000000000000010; readback | 0x2000000000000010 on all 3 runs (field=1, exactly the written value, no other bit changed) |
| cbie=2 | write 0x2000000000000020; readback | 0x2000000000000020 on all 3 runs (field=2, exactly the written value) |
| cbie=3 | write 0x2000000000000030; readback | 0x2000000000000030 on all 3 runs (field=3, exactly the written value) |
| cbie=0 | write 0x2000000000000000; readback | 0x2000000000000000 on all 3 runs (field cleared exactly) |
| WARL | `csrw menvcfg, all-ones`; readback, twice | 0xa0000000000000f1 on both writes, on all 3 runs: CBIE field (bits 5:4) fully sticks, so the field is implemented; bits 63, 61, 7, 6, 0 also read back set; every other bit legalizes to 0 |
| restore | write boot value; readback | 0x2000000000000000 on all 3 runs (bit-for-bit, no WARL residue) |
| traps | handler record | 0 on all 3 runs |

Checks: 9 (CBIE=1 readback equals written, CBIE=2 readback equals
written, CBIE=3 readback equals written, CBIE=0 readback equals
written, WARL all-ones keeps CBIE field set, boot-set bits do not
vanish, two all-ones legalizations agree, restore reads back the
boot value bit-for-bit, trap count 0). Mismatches: 0. Checksum
0x695a0fc4812f73e3, identical on all 3 runs (stamped in the header
above). `RESULT: PASS` on all 3 runs.

Note: the WARL legalized value (0xa0000000000000f1) is this hart's
observed legalization and is reported, not asserted as portable.
The asserted properties are exactly the three checked above: the
CBIE field survives an all-ones write, boot-set bits do not vanish,
and the legalization is stable across two writes.
