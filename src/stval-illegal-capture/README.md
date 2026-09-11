# stval-illegal-capture

Executes the illegal word 0xffffffff in S-mode under QEMU, takes
the delegated illegal-instruction trap, and verifies what the hart
writes to stval: scause == 2 and stval equal to the executed
encoding, then resumes past the word.

## Layout

- `stvc_trap.S`: M-mode and S-mode trap entries. The S-mode entry
  records scause/sepc/stval at entry, advances sepc by 4, raises
  the done flag, and sret's. The M-mode entry only records; no
  M-mode trap is expected.
- `stvc_main.c`: M-mode setup (medeleg bit 2 write/readback
  triple, trap vectors, PMP opening the address space, interrupt
  enables cleared), the drop to S-mode, the labeled illegal word,
  the seven checks, the quiet window, and the PASS/FAIL verdict
  with finisher shutdown.
- `bench-logs/`: the captured build log and the three QEMU run
  logs.
- `PROOF.md`: the measured record with the machine-readable
  proof header.

## Build and run

```
CROSS=<path-to>/riscv64-unknown-elf- make stval-illegal-capture.elf
QEMU=<qemu-8.2.2>/qemu-system-riscv64 make run-stval-illegal-capture
```

The run ends with `RESULT: PASS (checks=7)` and shuts the machine
down (QEMU exits 0). If a check fails, the hart parks instead of
shutting down; the failing check is printed with a `FAIL:` line.
