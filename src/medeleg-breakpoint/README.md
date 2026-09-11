# medeleg-breakpoint-route

Checks where a breakpoint trap raised in S-mode lands as `medeleg`
bit 3 flips, under QEMU 8.2.2 in M-mode.

Phase 1 writes 0 to `medeleg` (readback `0x0`, bit 3 verified
clear), drops to S-mode, and executes an `ebreak`: the trap must
land in M-mode with `mcause = 3` and `mepc` exactly at the ebreak.
The M-mode handler records the cause, PC, status, and trap value,
then redirects into the phase-2 setup. Phase 2 sets `medeleg` bit 3
(readback exactly `0x8`), drops to S-mode, and executes the same
ebreak: the trap must land in S-mode with `scause = 3` and `sepc`
exactly at the ebreak, resuming after it. The run publishes the
per-mode trap counts, causes, PCs, trap values, and both `medeleg`
write/readback pairs, runs 14 checks, and writes the FNV-1a
checksum over the verdict values so repeated runs can be compared
byte for byte. The ebreak is emitted as `.word 0x00100073` so it
stays the 4-byte encoding under RVC.

Files:

- `mebk_main.c`: two-phase driver, UART reporting, checks, checksum,
  PASS/FAIL verdict with virt test-device finisher shutdown on PASS
  and a parked hart on FAIL.
- `mebk_trap.S`: M-mode and S-mode trap entries (record, count, and
  resume/redirect).
- `PROOF.md`: proof log with the machine-readable header, the
  measured values, and the verification record.
- `bench-logs/`: build log and the three raw QEMU run logs.

Build and run from the repo root (CROSS to taste):

    make medeleg-breakpoint.elf
    make run-medeleg-breakpoint

On a passing run QEMU exits 0 after `RESULT: PASS (checks=14)`.
