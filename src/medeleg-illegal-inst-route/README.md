# medeleg-illegal-inst-route

Checks where an illegal-instruction trap raised in S-mode lands as
`medeleg` bit 2 flips, under QEMU 8.2.2 in M-mode.

Phase 1 writes 0 to `medeleg` (readback `0x0`, bit 2 verified
clear), drops to S-mode, and executes a `0xffffffff` illegal word:
the trap must land in M-mode with `mcause = 2` and `mepc` exactly at
the word. The M-mode handler records the cause, PC, status, and trap
value, then redirects into the phase-2 setup. Phase 2 sets `medeleg`
bit 2 (readback `0x4`), drops to S-mode, and executes the same word:
the trap must land in S-mode with `scause = 2` and `sepc` exactly at
the word, resuming after it. The run publishes the per-mode trap
counts, causes, PCs, trap values, and both `medeleg` write/readback
pairs, runs 13 checks, and writes the FNV-1a checksum over the
verdict values so repeated runs can be compared byte for byte.

Files:

- `mili_main.c`: two-phase driver, UART reporting, checks, checksum,
  PASS/FAIL verdict with virt test-device finisher shutdown on PASS
  and a parked hart on FAIL.
- `mili_trap.S`: M-mode and S-mode trap entries (record, count, and
  resume/redirect).
- `PROOF.md`: proof log with the machine-readable header, the
  measured values, and the verification record.
- `bench-logs/`: build log and the three raw QEMU run logs.

Build and run from the repo root (CROSS to taste):

    make medeleg-illegal-inst-route.elf
    make run-medeleg-illegal-inst-route

On a passing run QEMU exits 0 after `RESULT: PASS (checks=13)`.
