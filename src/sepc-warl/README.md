# sepc WARL software-write probe (backlog item 123)

Measures which bits of the `sepc` CSR are software-writable on the
hart, and checks what an M-mode trap does to the register.

## What it does

1. Installs a minimal M-mode trap handler (direct-mode `mtvec`,
   `mscratch` scratch area) that records `mcause`/`sepc`/`mepc`,
   bumps a trap counter, and resumes past the 4-byte `ecall`.
2. Writes all-ones to `sepc` with `csrw` and publishes the
   write/readback pair; requires the readback to equal all-ones
   (every XLEN bit stuck) and the trap count to be unchanged.
3. Writes zero to `sepc`; requires the readback to equal zero (no
   bits are read-only).
4. Issues a deliberate M-mode `ecall` (cause 11). Per the
   privileged spec, an M-mode trap writes `mepc` with the trap pc,
   while `sepc` is written only when a trap is taken into S-mode,
   so `sepc` must still hold the software-written 0. Requires
   trap count 1, the handler-recorded `mepc` to equal the ecall pc,
   the handler-recorded and independently read-back `sepc` to
   equal 0, and the post-handler `mepc` readback to equal
   ecall pc + 4.

Measured finding: on this hart (QEMU 8.2.2, virt machine) all 64
bits of `sepc` are software-writable, and an M-mode trap leaves
the register untouched (the write of 0 survived the ecall).

## Files

- `sepc_main.c`: the test sequence, UART reporting, PASS/FAIL
  verdict, and the virt test-device finisher shutdown.
- `sepc_trap.S`: the trap entry.
- `PROOF.md`: the full proof record with per-run measurements.
- `bench-logs/`: the build log and the three raw QEMU run logs.

## Build and run

From the repo root:

```
make sepc-warl.elf
make run-sepc-warl
```

PASS is reported as `RESULT: PASS (traps=1)` and the finisher
shuts the machine down, so QEMU exits 0. FAIL parks the hart in a
`wfi` loop without touching the finisher; under `timeout` that
shows up as exit status 124.
