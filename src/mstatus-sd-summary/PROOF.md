<!-- PROOF-HEADER -->
<!-- Checks: 15 -->
<!-- Mismatches: 0 -->
<!-- Checksum: 0xcd8421132251ca67 -->
<!-- Environment: QEMU 8.2.2 -->
<!-- Verdict: PASS -->

# PROOF: mstatus-sd-summary

The header block above was stamped at run time from the real UART output of
the QEMU run (run1); the same numbers appear verbatim in all three raw logs
in `bench-logs/`.

## What was built

A bare-metal RISC-V module (`src/mstatus-sd-summary/`, its own ELF sharing
only `src/boot.S` and `src/uart.c` with the other demos) that proves, in
M-mode on the QEMU `virt` board, that `mstatus.SD` (bit 63) is the read-only
summary bit for the FS field (bits 14:13): 1 exactly when FS reads Dirty,
never settable by software. Every `mstatus` write is read-modify-write
(`csrc`/`csrs`) preserving all other bits, except the deliberate bit-63
probe and the final explicit restore. A counting M-mode trap handler
(`ssd_trap.S`) is installed as hygiene and records/parts on any unexpected
trap; the run requires the counter to stay 0.

The program issues, in order: boot readback, FS=Off write, FS=Initial write,
`fadd.d f0,f1,f2` on 1.5 and 2.25 (loaded as integer bit patterns via
`fmv.d.x`, read back via `fmv.x.d`; no FP literal pools), FS=Clean write, a
forced `csrs` of bit 63, the boot-word restore, and the trap-count check.
On PASS the machine shuts down through the virt test-device finisher so
QEMU exits 0; on FAIL the hart parks in `wfi` (observable as the harness's
timeout exit status 124).

## Files

- `src/mstatus-sd-summary/ssd_main.c` - the experiment and its 15 numbered checks
- `src/mstatus-sd-summary/ssd_trap.S` - counting M-mode trap entry (parks on any trap)
- `src/mstatus-sd-summary/README.md` - one-paragraph module description
- `src/mstatus-sd-summary/build.sh` - standalone build (same flags as the repo Makefile)
- `src/mstatus-sd-summary/bench-logs/build.log` - the real build log
- `src/mstatus-sd-summary/bench-logs/run1.log`, `run2.log`, `run3.log` - raw UART logs

## Measured readbacks (all 3 runs, verbatim from run1.log)

| Step | mstatus readback | FS | SD | Note |
|---|---|---|---|---|
| boot | 0xa00000000 | 0 (Off) | 0 | bits 35/33 set are SXL/UXL=2 (64-bit) |
| fs-off write | 0xa00000000 | 0 (Off) | 0 | |
| fs-initial write | 0xa00004000 | 2 (Initial) | 0 | SD stays 0: enabled but not dirty |
| after fadd.d | 0x8000000a00006000 | 3 (Dirty) | 1 | f0 bits = 0x400e000000000000 (3.75) |
| fs-clean write | 0xa00002000 | 1 (Clean) | 0 | SD falls back to 0 |
| bit-63 probe | 0xa00002000 | 1 (Clean) | 0 | forced bit-63 write ignored: read-only |
| restore | 0xa00000000 | 0 (Off) | 0 | equals boot word bit-for-bit |

Full run1 UART output:

```
mstatus-sd-summary: mstatus.SD tracks the FS Dirty state
setup: mtvec=0x80000718
boot: mstatus=0xa00000000 FS=0 SD=0 (expect FS=0 SD=0)
fs-off: mstatus=0xa00000000 FS=0 SD=0 (expect FS=0 SD=0)
fs-initial: mstatus=0xa00004000 FS=2 SD=0 (expect FS=2 SD=0)
after-fadd: mstatus=0x8000000a00006000 FS=3 SD=1 f0bits=0x400e000000000000
fs-clean: mstatus=0xa00002000 FS=1 SD=0 (expect FS=1 SD=0)
sd-probe: mstatus=0xa00002000 SD=0 FS=1 (expect SD=0 FS=1)
restore: mstatus=0xa00000000 (expect boot word)
traps=0
checks=15 mismatches=0
checksum=0xcd8421132251ca67
RESULT: PASS
```

## The 15 checks

1. boot mstatus FS field == 0 (Off)
2. boot mstatus SD == 0
3. FS reads back Off after the FS=Off write
4. SD reads 0 with FS=Off
5. FS reads back Initial after the FS=Initial write
6. SD reads 0 with FS=Initial (not Dirty)
7. fadd.d moved FS to Dirty (field reads 3)
8. SD reads 1 with FS=Dirty
9. f0 reads back exactly 0x400e000000000000 (1.5 + 2.25 = 3.75: the FPU really executed)
10. FS reads back Clean after the FS=Clean write
11. SD reads 0 after FS=Clean cleared it
12. SD reads 0 after forcing bit 63 to 1 (summary bit is read-only)
13. FS unchanged across the SD probe write
14. restore readback equals the boot word bit-for-bit
15. trap count == 0

## Byte identity

Three QEMU 8.2.2 runs under `timeout 30`, all exit code 0:

```
0a2ba038adab5405ee613aaef8c643a5  run1.log
0a2ba038adab5405ee613aaef8c643a5  run2.log
0a2ba038adab5405ee613aaef8c643a5  run3.log
```

Byte-identical (`md5sum`). The checksum is FNV-1a 64 over the check-record
words (boot, Off, Initial, post-fadd, f0 bits, Clean, probe, restored,
trap count); no host timing or absolute data enters.

## Limits of verification

Verified on the QEMU 8.2.2 `virt` emulator with the D extension, not on
silicon: the SD summary rule (SD=1 exactly when FS or XS is Dirty, bit 63
read-only) is a property of the RISC-V privileged specification that QEMU
models faithfully, but no real hart was exercised. Single hart only;
interrupt behavior outside M-mode is out of scope (the module never drops
out of M-mode and never enables interrupts).
