<!-- PROOF-HEADER
Checks: 9
Mismatches: 0
Checksum: 0x5073d6e9f997f3a3
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: menvcfg.CBZE reads back exactly as written (0, 1, 2, 3, WARL legalize)

## What was built

`src/menvcfg-cbze-write/`: a bare-metal RISC-V program that verifies,
on the QEMU `virt` board, the write/readback contract of
`menvcfg.CBZE` (bits 7:6), the WARL field that governs the
cache-block-zero extension behavior. The experiment runs entirely in
M-mode on hart 0 (QEMU boots the ELF straight into M-mode with
`-bios none`, so no privilege drop is needed), and the module shares
only `src/boot.S` and `src/uart.c` with the other demos.

- `mcw_main.c`: prints the boot `menvcfg` value; writes each CBZE
  value 1, 2, 3 with all other bits held at their boot values and
  requires each readback to equal the written value exactly (the
  field takes every legal encoding, no other bit moved); writes
  CBZE=0 and requires the exact readback; writes all-ones twice and
  publishes both legalized readbacks, asserting only what the spec
  and this hart guarantee (the CBZE field survives the write, bits
  set at boot do not vanish, the two legalizations agree) and
  reporting the rest; restores the boot value and requires the
  exact readback; prints and requires a zero trap count. A 64-bit
  FNV-1a checksum is fed the boot value, the four CBZE
  write/readback pairs, the two legalized all-ones readbacks, and
  the restore readback in a fixed order and printed on the
  completion path; it carries no absolute addresses or live
  counters, so it is identical across runs.
- `mcw_trap.S`: minimal M-mode trap entry (direct mode). Any trap
  during this experiment is unexpected (the program never issues a
  trapping instruction), so the handler records
  `mcause`/`mepc`/`mtval` and a trap count into `mcw_save`, then
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
-kernel menvcfg-cbze-write.elf` under `timeout` so a parked-hart
FAIL is observable as exit status 124. QEMU is the 8.2.2 build
from `~/workspace/qemu` (system `qemu-system-riscv64` on this VM
fails with a missing `libfdt.so.1`).

## Configuration under test

- Hart: hart 0, single hart, M-mode throughout. QEMU boots the ELF
  straight into M-mode with `-bios none`.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`),
  `riscv64-unknown-elf-gcc` 15.2.0 (xPack), `-march=rv64imac_zicsr`.
- Host CPU: AMD EPYC 9D25 126-Core Processor
  (`bench-logs/host-cpu.txt`).
- `mtvec` points at the park-on-trap handler; no delegation (M-mode
  keeps every trap).

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs exited 0 via the test-device finisher, and the three logs are
byte-identical (md5 `1c76d730eab66792a93f98daf513178e` on all three).

| step | operation | measured readback |
|---|---|---|
| boot | `csrr menvcfg` | 0x2000000000000000, CBZE(bits7:6)=0, on all 3 runs |
| cbze=1 | write 0x2000000000000040; readback | 0x2000000000000040 on all 3 runs (field=1, exactly the written value, no other bit changed) |
| cbze=2 | write 0x2000000000000080; readback | 0x2000000000000080 on all 3 runs (field=2, exactly the written value) |
| cbze=3 | write 0x20000000000000c0; readback | 0x20000000000000c0 on all 3 runs (field=3, exactly the written value) |
| cbze=0 | write 0x2000000000000000; readback | 0x2000000000000000 on all 3 runs (field cleared exactly) |
| WARL | `csrw menvcfg, all-ones`; readback, twice | 0xa0000000000000f1 on both writes, on all 3 runs: CBZE field (bits 7:6) fully sticks, so the field is implemented; bits 63, 61, 5, 4, 0 also read back set; every other bit legalizes to 0 |
| restore | write boot value; readback | 0x2000000000000000 on all 3 runs (bit-for-bit, no WARL residue) |
| traps | handler record | 0 on all 3 runs |

Checks: 9 (CBZE=1 readback equals written, CBZE=2 readback equals
written, CBZE=3 readback equals written, CBZE=0 readback equals
written, WARL all-ones keeps CBZE field set, boot-set bits do not
vanish, two all-ones legalizations agree, restore reads back the
boot value bit-for-bit, trap count 0). Mismatches: 0. Checksum
0x5073d6e9f997f3a3, identical on all 3 runs (stamped in the header
above). `RESULT: PASS` on all 3 runs.

Note: the WARL legalized value (0xa0000000000000f1) is this hart's
observed legalization and is reported, not asserted as portable.
The asserted properties are exactly the three checked above: the
CBZE field survives an all-ones write, boot-set bits do not vanish,
and the legalization is stable across two writes.
