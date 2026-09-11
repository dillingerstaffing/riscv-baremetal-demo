<!-- PROOF-HEADER
Checks: 5
Mismatches: 0
Checksum: 0x5ef757269922ecb1
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mstatus.MPRV reads back exactly as written (set via csrs, clear via csrc)

## What was built

`src/mstatus-mprv-readback/`: a bare-metal RISC-V program that
verifies, on the QEMU `virt` board, the write/readback contract of
`mstatus.MPRV` (bit 17), the M-mode bit that makes data loads and
stores use the MPP privilege instead of the current mode. The
experiment runs entirely in M-mode on hart 0 (QEMU boots the ELF
straight into M-mode with `-bios none`, so no privilege drop is
needed), and the module shares only `src/boot.S` and `src/uart.c`
with the other demos.

- `mprv_main.c`: prints the boot `mstatus`; runs the set/probe/clear
  sequence inside ONE asm block that executes only CSR instructions
  between the `csrs` and the `csrc` (a hazard the test avoids on
  purpose: with MPRV=1 and boot MPP=U and no PMP entries
  programmed, a data load or store is privilege-checked as a U-mode
  access and faults, so the window between set and clear is kept
  memory-free); requires the set readback to carry bit 17, and
  separately requires it to equal boot|MPRV exactly (no other bit
  disturbed); requires the clear readback to equal the boot value
  exactly; reads `mstatus` once more after the probes and requires
  it to still equal the boot value (no residue); prints and requires
  a zero trap count. A 64-bit FNV-1a checksum is fed the boot value,
  both readbacks, and the post-run value in a fixed order and
  printed on the completion path; it carries no absolute addresses
  or live counters, so it is identical across runs.
- `mprv_trap.S`: minimal M-mode trap entry (direct mode). Any trap
  during this experiment is unexpected (no trapping instruction is
  issued in M-mode with the vector armed), so the handler records
  `mcause`/`mepc`/`mtval` and a trap count into `mprv_save`, then
  parks the hart in a `wfi` loop. Reaching the printed verdict
  already implies zero traps, and the program also checks the count
  explicitly.
- `build.sh`, `PROOF.md` (this file), `bench-logs/` with the build
  log, three raw QEMU run logs, and `host-cpu.txt`.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 5 checks held. On PASS the machine is shut
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
-kernel mstatus-mprv-readback.elf` under `timeout` so a parked-hart
FAIL is observable as exit status 124. QEMU is the 8.2.2 build
from `~/workspace/qemu` (the compat-bin qemu is broken on this VM).

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
byte-identical.

| step | operation | measured readback |
|---|---|---|
| boot | `csrr mstatus` | 0xa00000000, MPRV(bit17)=0, MPP=0 (U), on all 3 runs |
| set | `csrs mstatus`, bit 17; readback | 0xa00020000 on all 3 runs: bit 17 set, and readback == boot\|MPRV exactly (no other bit changed) |
| clear | `csrc mstatus`, bit 17; readback | 0xa00000000 on all 3 runs, exactly the boot value |
| restore | `csrr mstatus` after the probes | 0xa00000000 on all 3 runs (no residue) |
| traps | handler record | 0 on all 3 runs |

Checks: 5 (set readback has bit 17 set; set readback equals
boot|MPRV with no other bit disturbed; clear readback equals the
boot value exactly; post-run mstatus equals the boot value; trap
count 0). Mismatches: 0. Checksum 0x5ef757269922ecb1, identical on
all 3 runs (stamped in the header above). `RESULT: PASS` on all 3
runs.

Note on the boot value: 0xa00000000 is this board's observed boot
`mstatus` (bits 35 and 33 set: SXL[1] and UXL[1], the 64-bit
XLEN fields), reported as measured; no portability claim is made
about it.
