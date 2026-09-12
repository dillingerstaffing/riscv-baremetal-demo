<!-- PROOF-HEADER
Checks: 6
Mismatches: 0
Checksum: 0xb162675eda645223
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: menvcfg.PBMTE write/readback contract (WARL legalization)

## What was built

`src/menvcfg-pbmte-write/`: a bare-metal RISC-V program that verifies, on
the QEMU `virt` board, the write/readback contract of `menvcfg.PBMTE`
(bit 62), the M-mode switch for Svpbmt page-based memory types. `menvcfg`
is WARL, so the experiment does not assume what bit 62 does: it writes
values and reads back the hart's legalized value. The experiment runs
entirely in M-mode on hart 0 (QEMU boots the ELF straight into M-mode
with `-bios none`, so no privilege drop is needed), and the module
shares only `src/boot.S` and `src/uart.c` with the other demos.

- `mpw_main.c`: prints the boot `menvcfg` value; writes boot|PBMTE
  and records whether bit 62 stuck (the set probe proves PBMTE is not
  implemented on this hart: it legalizes to 0), while requiring that
  no other bit changed under the write; writes boot&~PBMTE and
  requires the exact readback; writes all-ones twice and publishes
  both legalized readbacks, asserting only what the spec and this hart
  guarantee (bits set at boot do not vanish, the two legalizations
  agree) and reporting the rest; the PBMTE-survival check is applied
  only when the set probe proved the bit implemented (not the case
  here, and the program says so); restores the boot value and
  requires the exact readback; prints and requires a zero trap count.
  A 64-bit FNV-1a checksum is fed the boot value and the
  write/readback pairs in a fixed order and printed on the completion
  path; it carries no absolute addresses or live counters, so it is
  identical across runs.
- `mpw_trap.S`: minimal M-mode trap entry (direct mode). Any trap
  during this experiment is unexpected (the program never issues a
  trapping instruction), so the handler records
  `mcause`/`mepc`/`mtval` and a trap count into `mpw_save`, then
  parks the hart in a `wfi` loop. Reaching the printed verdict
  already implies zero traps, and the program also checks the
  count explicitly.
- `build.sh`, `PROOF.md` (this file), `bench-logs/` with the build
  log, three raw QEMU run logs, and `host-cpu.txt`.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 6 checks held. On PASS the machine is shut
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
-kernel menvcfg-pbmte-write.elf` under `timeout` so a parked-hart
FAIL is observable as exit status 124. QEMU is the 8.2.2 build
from `~/workspace/qemu` (system `qemu-system-riscv64` on this VM
fails with a missing `libfdt.so.1`). Toolchain: Ubuntu
`gcc-riscv64-unknown-elf` 13.2.0 from `~/workspace/toolchains/ubuntu-rv64`.

## Configuration under test

- Hart: hart 0, single hart, M-mode throughout. QEMU boots the ELF
  straight into M-mode with `-bios none`.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`),
  `riscv64-unknown-elf-gcc` 13.2.0 (Ubuntu), `-march=rv64imac_zicsr`.
- Host CPU: AMD EPYC 9D25 126-Core Processor
  (`bench-logs/host-cpu.txt`).
- `mtvec` points at the park-on-trap handler; no delegation (M-mode
  keeps every trap).

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs exited 0 via the test-device finisher, and the three logs are
byte-identical (md5 a5266af4f7477445265d4dfc9858284c for all three).

| step | operation | measured readback |
|---|---|---|
| boot | `csrr menvcfg` | 0x2000000000000000, PBMTE(bit62)=0, on all 3 runs |
| set | write boot\|PBMTE = 0x6000000000000000; readback | 0x2000000000000000 on all 3 runs: bit 62 does NOT stick, so PBMTE is not implemented on this hart; every other bit is unchanged under the write |
| clear | write boot&~PBMTE = 0x2000000000000000; readback | 0x2000000000000000 on all 3 runs (exactly the written value) |
| WARL | `csrw menvcfg, all-ones`; readback, twice | 0xa0000000000000f1 on both writes, on all 3 runs: STCE (bit 63) survives; bit 62 reads back 0, consistent with the set-probe finding that PBMTE is not implemented; bits 61, 7, 6, 5, 4, 0 also read back set; every other bit legalizes to 0 |
| restore | write boot value; readback | 0x2000000000000000 on all 3 runs (no WARL residue) |
| traps | handler record | 0 on all 3 runs |

Checks: 6 (PBMTE set probe leaves every other bit unchanged, PBMTE
clear probe readback equals write, WARL all-ones keeps boot-set bits,
two all-ones legalizations agree, restore reads back the boot value,
trap count 0). Mismatches: 0. Checksum 0xb162675eda645223, identical
on all 3 runs (stamped in the header above; recomputed from the
published readbacks in `bench-logs/`). `RESULT: PASS` on all 3 runs.

Note: the legalized values (0x2000000000000000 for the boot round-trip
and 0xa0000000000000f1 for the all-ones WARL probe) are this hart's
observed legalizations and are reported, not asserted as portable.
The asserted properties are exactly the six checked above. In
particular, bit 62's refusal to stick is a measured property of this
hart (WARL legalization to 0), and the program's WARL probe is
written so the same code would assert PBMTE survival on a hart where
the set probe shows the bit implemented.
