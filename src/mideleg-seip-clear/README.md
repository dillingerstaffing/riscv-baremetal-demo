# mideleg SEIP clear-and-restore (backlog item "riscv mideleg-seip-clear")

Sibling of `src/mideleg-seip-route`, which verified the set path
only. This module clears `mideleg` bit 9 after the supervisor
external interrupt is pending and requires the next delivery to
move from S-mode to M-mode, then re-sets the bit and requires
delivery to move back to S-mode.

## Phases

- Phase A (control, replicates the sibling): `mideleg` bit 9 set,
  SEIP pended from M-mode, drop to S-mode. Exactly one S-mode trap
  with `scause = 0x8000000000000009` and 0 M-mode traps.
- Phase B (the claim under test): pend SEIP again, clear
  `mideleg` bit 9, enable the M-mode path (`mie.SEIE`, then
  `mstatus.MIE`). Exactly one M-mode trap with
  `mcause = 0x8000000000000009` (the SEI source keeps code 9;
  delegation selects the taker, not the code) and 0 new S-mode
  traps.
- Phase C (re-set): re-set bit 9, pend SEIP again, drop to
  S-mode. Exactly one more S-mode trap (`s_traps = 2`).
- Phase D: clear the pending SEIP from M-mode, verify the enables
  are off, run a 2,000,000-cycle quiet window with the counters
  frozen, and restore `mideleg` and `mstatus` to their boot
  readbacks bit-for-bit.

## How the S-mode phases return to M-mode

`medeleg` is 0, so an S-mode `ecall` traps in M-mode with
`mcause = 9`. The M-mode handler does not return to S-mode for
this case: it bumps a separate ecall counter (slot 7, kept apart
from the M-mode interrupt counter in slot 0) and jumps `mepc` to a
trampoline target the C code set, then `mret`. Phase A hands to
`phase_b`, phase C hands to `phase_d`.

## Why the M-mode handler masks MIE and MPIE

The SEI under test is level-triggered and stays pending through
the phase-B trap (S-mode could never clear it, and M-mode keeps it
pending until phase D). `mret` restores MIE from MPIE, so clearing
MIE alone would be undone on return and the interrupt would
re-fire immediately; the handler clears both.

## Backlog corrections

The backlog line said "pend STIP via a sip write" but named
`mcause = 0xb` and `scause = 0x8000000000000009`, conflating the
timer source (STIP is code 5) with the external one; this module
pends SEIP with an M-mode `csrs mip, 1<<9` like the sibling.
Separately, the backlog's `mcause = 0xb` is wrong for the phase-B
trap: the pending source is `mip.SEIP` (bit 9), and clearing its
delegation moves the trap to M-mode without changing the source's
code, so the measured `mcause` is `0x8000000000000009`. PROOF.md
documents both corrections.

## Files

- `seip_clear_main.c`: the four-phase sequence, UART reporting,
  PASS/FAIL verdict, and the virt test-device finisher shutdown.
- `seip_clear_trap.S`: the M-mode trap entry (records
  `mcause`/`mepc`; interrupts bump slot 0 and mask MIE+MPIE;
  S-mode ecalls bump slot 7 and jump to the trampoline target)
  and the S-mode entry (counts, records `scause`/`sepc`/`sip`-at-entry,
  masks via SIE/SPIE, raises the done flag, `sret`).
- `PROOF.md`: the full proof record with per-run measurements.
- `bench-logs/`: the build log and the three raw QEMU run logs.

## Build and run

From the repo root:

```
make mideleg-seip-clear.elf
make run-mideleg-seip-clear
```

PASS is reported as `RESULT: PASS (checks=50)` and the finisher
shuts the machine down, so QEMU exits 0. (The count covers every
`check()` in the program: 13 in main, 8 in phase A, 15 in phase B,
7 in phase C, 7 in phase D. If the count differs, the binary and
this README disagree and the run is not trustworthy.) FAIL parks
the hart in a `wfi` loop without touching the finisher; under
`timeout` that shows up as exit status 124.
