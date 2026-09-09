# PROOF: mstatus.FS field write/readback measurement (backlog item 80)

Backlog item 80: in M-mode, write the FS field (bits 14:13) of
`mstatus` through all four values (0..3), read back `mstatus` after
each write, and verify (a) the FS field reads back the written
value and (b) no other `mstatus` bit changed relative to a baseline
captured before the writes. Publish the write/readback pairs over
3 runs.

## What was built

`src/fs-check/fs_main.c`, a bare-metal M-mode binary sharing only
`src/boot.S` and the UART driver with the other demos. It:

1. Captures a baseline `mstatus` in M-mode right after boot.
2. Writes the FS field through 0, 1, 2, 3 (each write changes only
   bits 14:13 of the baseline word), reading back the full
   `mstatus` word after each write with plain `csrr`/`csrw`.
3. Repeats the full 0..3 pass a second time, so FS readback
   consistency for a given written value is measured, not assumed.
4. Restores the baseline word, then prints `RESULT: PASS` or
   `RESULT: FAIL`.
5. Encodes the verdict in the program exit path: on PASS it writes
   the virt test-device finisher word `0x5555` at `0x100000`, which
   shuts the machine down (QEMU exits 0); on FAIL it parks the hart
   in a `wfi` loop without touching the finisher, so under the
   harness's `timeout` a FAIL is observable as exit status 124 as
   well as the `RESULT: FAIL` line.

Build integration: `Makefile` gains `fs-check.elf`, `run-fs-check`,
and the module is in `all` and `clean`.

## Checks (all computed, none eyeballed)

1. FS readback equals the written value on all 8 writes.
2. No bit outside the FS field and bit 63 changed relative to the
   baseline on any write.
3. Bit 63 (SD) reads 1 exactly when FS was written as 3, and 0
   otherwise (this is the measured machine behavior; see below).
4. The FS readback for a given written value is identical in both
   iterations.

## Measured write/readback table

QEMU 8.2.2, `-machine virt`, 3 runs. Baseline `mstatus` at boot in
M-mode: `0xa00000000` (bits 33 and 35 set: 64-bit SXL/UXL fields;
FS reads as 0, SD reads as 0). All 3 runs byte-identical:

| iteration | wrote FS | readback `mstatus`   | FS readback | SD |
|-----------|----------|----------------------|-------------|----|
| 0         | 0        | 0xa00000000          | 0           | 0  |
| 0         | 1        | 0xa00002000          | 1           | 0  |
| 0         | 2        | 0xa00004000          | 2           | 0  |
| 0         | 3        | 0x8000000a00006000   | 3           | 1  |
| 1         | 0        | 0xa00000000          | 0           | 0  |
| 1         | 1        | 0xa00002000          | 1           | 0  |
| 1         | 2        | 0xa00004000          | 2           | 0  |
| 1         | 3        | 0x8000000a00006000   | 3           | 1  |

Verdict lines from the logs: FS readback == written value on all
writes: yes; non-FS non-SD bits unchanged across all writes: yes;
FS readback consistent across iterations: yes. `RESULT: PASS` on
all 3 runs, QEMU exit code 0 on all 3 runs.

## Why the SD bit moved (source verification, QEMU 8.2.2)

The only non-FS bit that changed during any write was bit 63
(SD), and only on the FS=3 writes: readback
`0x8000000a00006000` differs from the expected `0xa00006000` by
exactly that bit. In QEMU's `write_mstatus`
(target/riscv/csr.c), after the masked write the emulator
recomputes SD as the OR-reduction of the dirty extension states:
`SD = 1` exactly when FS == 3 (or XS == 3). That matches the
RISC-V privileged specification's definition of SD, and it is why
check 3 above reads SD = 1 exactly for the FS=3 writes. This was
found by measurement first (an earlier build of this module failed
its "no other bit changes" check on the FS=3 writes), then
confirmed against the QEMU source.

## Limits of verification

- Emulator, not silicon: every number above is a property of
  QEMU 8.2.2's `write_mstatus`/`read_mstatus` implementation on
  this host, not of physical RISC-V hardware. A real core may
  hardwire FS, WARL it, or implement the F extension.
- QEMU version: the SD-recompute behavior was read in the QEMU
  8.2.2 source tree; other versions may differ.
- Host effects: none observed (the table is deterministic and
  byte-identical across 3 runs), but scheduling stalls can only
  affect timing-sensitive modules, not CSR readback.
- Scope: this module measures only the `mstatus` FS field's
  write/readback behavior in M-mode. It does not test the F
  extension itself (no floating-point instruction is executed),
  and it does not test `sstatus`/VS-mode behavior.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/fs-check/fs_main.c -o src/fs-check/fs_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o fs-check.elf src/boot.o src/uart.o src/fs-check/fs_main.o
ld: warning: fs-check.elf has a LOAD segment with RWX permissions
```

The RWX warning comes from the shared `link.ld` (used by every
module in this repo) and is present in the other modules' build
logs too; no new warning was introduced by this module.

## Run logs (QEMU 8.2.2, all 3 runs)

Run 1 (run1.log):
```
fs-check: mstatus.FS field write/readback measurement
baseline mstatus: 0xa00000000 (FS bits read as 0)
iteration 0:
  wrote FS=0 readback=0xa00000000 FS_readback=0 SD=0
  wrote FS=1 readback=0xa00002000 FS_readback=1 SD=0
  wrote FS=2 readback=0xa00004000 FS_readback=2 SD=0
  wrote FS=3 readback=0x8000000a00006000 FS_readback=3 SD=1
iteration 1:
  wrote FS=0 readback=0xa00000000 FS_readback=0 SD=0
  wrote FS=1 readback=0xa00002000 FS_readback=1 SD=0
  wrote FS=2 readback=0xa00004000 FS_readback=2 SD=0
  wrote FS=3 readback=0x8000000a00006000 FS_readback=3 SD=1
FS readback == written value on all writes: yes
non-FS non-SD bits unchanged across all writes: yes
FS readback consistent across iterations: yes
RESULT: PASS
```

Runs 2 and 3 (run2.log, run3.log) are byte-identical to run 1.
QEMU exit code 0 on all 3 runs (finisher shutdown path).
