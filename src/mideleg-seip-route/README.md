# mideleg SEIP routing (backlog item "riscv mideleg-seip-route")

Publishes the `mideleg` write/readback triple for bit 9 (the write
takes on this hart), pends the supervisor external interrupt from
M-mode with `csrs mip, 1<<9`, drops to S-mode, and requires exactly
one S-mode trap with `scause = 0x8000000000000009` and zero M-mode
traps.

## Why the pend happens in M-mode

`sip` bit 9 (SEIP) is read-only for S-mode: the shipped
`src/sip-seip-write` module measured that an S-mode all-ones write
to `sip` leaves SEIP unchanged. An M-mode `csrs mip, 1<<9` does set
the bit (readback-verified in this module), so the pend happens in
M-mode before the drop.

## Why the handler masks instead of clearing

The S-mode handler cannot clear SEIP (read-only from S-mode, same
finding as above). It records `scause`/`sepc`/the `sip` value at
entry, clears `sstatus.SIE` and `sstatus.SPIE`, raises the done
flag, and returns with `sret`. The SPIE clear is load-bearing:
`sret` restores SIE from SPIE, so clearing SIE alone would be undone
on return and the still-pending SEI would re-fire immediately. The
quiet window then verifies the counts never move.

## Files

- `seip_main.c`: the test sequence, UART reporting, PASS/FAIL
  verdict, and the virt test-device finisher shutdown.
- `seip_trap.S`: the M-mode trap entry (counts, records
  `mcause`/`mepc`, `mret`) and the S-mode entry (counts, records
  `scause`/`sepc`/`sip`-at-entry, masks via SIE/SPIE, raises the
  done flag, `sret`).
- `PROOF.md`: the full proof record with per-run measurements.
- `bench-logs/`: the build log and the three raw QEMU run logs.

## Build and run

From the repo root:

```
make mideleg-seip-route.elf
make run-mideleg-seip-route
```

PASS is reported as `RESULT: PASS (checks=22)` and the finisher
shuts the machine down, so QEMU exits 0. FAIL parks the hart in a
`wfi` loop without touching the finisher; under `timeout` that
shows up as exit status 124.
