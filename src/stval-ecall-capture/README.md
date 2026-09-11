# stval-ecall-capture

Drops from M-mode to U-mode with medeleg bit 8 set, issues one
ecall at a labeled site, and verifies what the hart writes to
stval on the environment-call trap: scause == 8 and stval == 0,
with exactly one S-mode trap and zero M-mode traps.

## Layout

- `sec_trap.S`: M-mode and S-mode trap entries. The S-mode entry
  records scause/sepc/stval at entry, advances sepc by 4, raises
  the done flag, and sret's back to U-mode. The M-mode entry only
  records; no M-mode trap is expected.
- `sec_main.c`: M-mode setup (medeleg bit 8 write/readback
  triple, trap vectors, PMP opening the address space, interrupt
  enables cleared), the drop to U-mode via sret with
  sstatus.SPP = 0, the labeled ecall, the eight checks, the quiet
  window, and the PASS/FAIL verdict with finisher shutdown.
- `bench-logs/`: the captured build log and the three QEMU run
  logs.
- `PROOF.md`: the measured record with the machine-readable
  proof header.

## Build and run

```
CROSS=<path-to>/riscv64-unknown-elf- make stval-ecall-capture.elf
QEMU=<qemu-8.2.2>/qemu-system-riscv64 make run-stval-ecall-capture
```

The run ends with `RESULT: PASS (checks=8)` and shuts the machine
down (QEMU exits 0). If a check fails, the hart parks instead of
shutting down; the failing check is printed with a `FAIL:` line.
