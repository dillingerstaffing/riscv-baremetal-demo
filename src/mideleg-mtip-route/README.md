# mideleg MTIP-routing check

Attempts to delegate the machine timer interrupt to S-mode via
`mideleg` bit 7, measures that the write is dropped by the WARL
legalization on this hart, then arms the CLINT machine timer, drops
to S-mode, and records exactly where the armed interrupt lands.

## What it does

1. Reads `mideleg` at boot: it reads 0x1444, the implementation's
   forced-on bits.
2. Installs the M-mode trap entry (direct-mode `mtvec`, `mscratch`
   scratch area) and the S-mode counting entry (direct-mode `stvec`,
   `sscratch`); the readbacks prove both vectors really took.
3. Publishes the write/readback triple: write 0, write 0x80 (bit 7
   only), write 0x1444|0x80. Every readback is 0x1444, so bit 7 is
   not admitted: the machine timer interrupt cannot be delegated to
   S-mode on this hart.
4. Sets `mie.MTIE` and `mstatus.MIE`, opens the address space to
   S-mode with one PMP NAPOT entry, grants S-mode the cycle/time
   counters via `mcounteren`, and drops to S-mode.
5. In S-mode, arms `mtimecmp` 500 mtime ticks ahead and waits for
   the M-mode handler to raise its done flag.
6. Requires exactly one M-mode trap with `mcause =
   0x8000000000000007`, zero S-mode traps, and `mtimecmp` disarmed
   (all-ones) by the in-handler write.
7. A quiet `rdcycle` window must leave the counts unchanged, then a
   64-bit FNV-1a checksum over the published record is printed.

Measured finding: on QEMU 8.2.2's virt hart, `mideleg` bit 7 is
read-only zero, so an armed machine timer still traps in M-mode
even when the hart is sitting in S-mode. One armed timer means one
trap, at the M-mode vector, with 0 S-mode arrivals.

## Files

- `mtip_main.c`: the test sequence, UART reporting, PASS/FAIL
  verdict, and the virt test-device finisher shutdown.
- `mtip_trap.S`: the M-mode trap entry (counts, records
  `mcause`/`mepc`, disarms the timer, raises the done flag,
  `mret`) and the S-mode counting entry (`sret`).
- `PROOF.md`: the full proof record with per-run measurements.
- `bench-logs/`: the build log and the three raw QEMU run logs.

## Build and run

From the repo root:

```
make mideleg-mtip-route.elf
make run-mideleg-mtip-route
```

PASS is reported as `RESULT: PASS (checks=20)` and the finisher
shuts the machine down, so QEMU exits 0. FAIL parks the hart in a
`wfi` loop without touching the finisher; under `timeout` that
shows up as exit status 124.
