<!-- PROOF-HEADER
Checks: 7
Mismatches: 0
Checksum: 0xc423e955a4446ba3
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: menvcfg.STCE reads back exactly as written (set, clear, WARL legalize)

## What was built

`src/menvcfg-stce-write/`: a bare-metal RISC-V program that verifies, on
the QEMU `virt` board, the write/readback contract of `menvcfg.STCE`
(bit 63), the M-mode switch that hands the Sstc timer to S-mode. The
experiment runs entirely in M-mode on hart 0 (QEMU boots the ELF
straight into M-mode with `-bios none`, so no privilege drop is
needed), and the module shares only `src/boot.S` and `src/uart.c` with
the other demos.

- `msw_main.c`: prints the boot `menvcfg` value; writes
  boot|STCE and requires the readback to equal the written value
  exactly (bit 63 sticks, no other bit changed); writes boot&~STCE
  and requires the exact readback; writes all-ones twice and
  publishes both legalized readbacks, asserting only what the spec
  and this hart guarantee (STCE survives the write, bits set at
  boot do not vanish, the two legalizations agree) and reporting
  the rest; restores the boot value and requires the exact
  readback; prints and requires a zero trap count. A 64-bit FNV-1a
  checksum is fed the boot value and the write/readback pairs in a
  fixed order and printed on the completion path; it carries no
  absolute addresses or live counters, so it is identical across
  runs.
- `msw_trap.S`: minimal M-mode trap entry (direct mode). Any trap
  during this experiment is unexpected (the program never issues a
  trapping instruction), so the handler records
  `mcause`/`mepc`/`mtval` and a trap count into `msw_save`, then
  parks the hart in a `wfi` loop. Reaching the printed verdict
  already implies zero traps, and the program also checks the
  count explicitly.
- `build.sh`, `PROOF.md` (this file), `bench-logs/` with the build
  log, three raw QEMU run logs, and `host-cpu.txt`.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 7 checks held. On PASS the machine is shut
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
-kernel menvcfg-stce-write.elf` under `timeout` so a parked-hart
FAIL is observable as exit status 124. QEMU is the 8.2.2 build
from `~/workspace/qemu` (system `qemu-system-riscv64` on this VM
fails with a missing `libfdt.so.1`).

## Configuration under test

- Hart: hart 0, single hart, M-mode throughout. QEMU boots the ELF
  straight into M-mode with `-bios none`.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`),
  `riscv64-unknown-elf-gcc` 15.2.0 (xPack), `-march=rv64imac_zicsr`.
- Host CPU: AMD EPYC 9D64 88-Core Processor
  (`bench-logs/host-cpu.txt`).
- `mtvec` points at the park-on-trap handler; no delegation (M-mode
  keeps every trap).

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs exited 0 via the test-device finisher, and the three logs are
byte-identical.

| step | operation | measured readback |
|---|---|---|
| boot | `csrr menvcfg` | 0x2000000000000000, STCE(bit63)=0, on all 3 runs |
| set | write boot\|STCE = 0xa000000000000000; readback | 0xa000000000000000 on all 3 runs (bit 63 set, no other bit changed) |
| clear | write boot&~STCE = 0x2000000000000000; readback | 0x2000000000000000 on all 3 runs (exactly the written value) |
| WARL | `csrw menvcfg, all-ones`; readback, twice | 0xa0000000000000f1 on both writes, on all 3 runs: STCE (bit 63) survives, so the bit is implemented; bits 61, 7, 6, 5, 4, 0 also read back set; every other bit legalizes to 0 |
| restore | write boot value; readback | 0x2000000000000000 on all 3 runs (no WARL residue) |
| traps | handler record | 0 on all 3 runs |

Checks: 7 (STCE set readback equals written, STCE clear readback
equals written, WARL all-ones keeps STCE set, boot-set bits do not
vanish, two all-ones legalizations agree, restore reads back the
boot value, trap count 0). Mismatches: 0. Checksum
0xc423e955a4446ba3, identical on all 3 runs (stamped in the header
above). `RESULT: PASS` on all 3 runs.

Note: the WARL legalized value (0xa0000000000000f1) is this hart's
observed legalization and is reported, not asserted as portable.
The asserted properties are exactly the three checked above: STCE
survives an all-ones write, boot-set bits do not vanish, and the
legalization is stable across two writes.
