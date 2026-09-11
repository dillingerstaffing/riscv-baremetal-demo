# sstatus.MXR gate on execute-only page loads (proof-backlog item "sstatus-mxr-probe")

Boots in M-mode on QEMU 8.2.2 (`virt`), builds a minimal Sv39 table by
hand (identity megapages for code/data/UART plus one 4 KiB
execute-only leaf mapping VA `0x40000000` to a canary page), delegates
the load page fault (`medeleg` bit 13) to S-mode, and `mret` drops to
S-mode. Phase A with `sstatus.MXR` clear does one `ld` from the
X-only VA and requires exactly one trap with `scause = 0xd`,
`stval = 0x40000000`, `sepc` equal to the faulting load (the handler
advances `sepc` by 4 and `sret`s; a t0 poisoned to 0 before the `ld`
proves the load never completed). Phase B sets MXR via `csrs` and
requires the same `ld` to return the canary with no new trap. On PASS
the machine shuts down via the virt test-device finisher (QEMU exit
0); on FAIL the hart parks.

This is the MXR counterpart to `src/sstatus-sum`: that module gates
S-mode loads from U-pages on the SUM bit; this one gates S-mode loads
from X-only supervisor pages on the MXR bit, a different control bit
and a different fault rule.

## What it does

1. M-mode: stores the canary `0xe4ec0d1edeadbeef` in the X-only page
   as a plain physical write (control: proves the page is good RAM
   before any table exists).
2. Builds the Sv39 tables: root[2] identity megapage
   `[0x80000000, 0xC0000000)` (code, data, stack, tables, supervisor,
   V|R|W|X); root[0] identity megapage `[0, 0x40000000)` (carries the
   UART at `0x10000000`); root[1] -> l1_test[0] -> l0_test[0] is a
   pure V-only pointer chain ending in a 4 KiB leaf mapping VA
   `0x40000000` to the canary page with V|X|A|D only (no R, no W,
   no U). All other entries invalid. Every table construction fact is
   read back and checked in-program.
3. Opens one PMP NAPOT R/W/X entry over the whole address space
   (read back `0x1f`), writes `medeleg` = bit 13 (load page fault
   delegated), installs direct-mode `stvec`, enables Sv39 via `satp`
   (read back MODE=8 with the root PPN), and drops to S-mode.
4. Phase A (MXR explicitly clear, read back 0): one `ld` from
   `0x40000000` at the global label `mxrs_fault_load`. Requires
   exactly one S-mode trap with `scause = 0xd`, `stval =
   0x40000000`, `sepc = mxrs_fault_load`; the stored load result must
   still be 0 (the `ld` never completed) and the resume marker 1.
5. Phase B (`csrs sstatus, MXR`, read back 1): the same `ld` must
   return the canary `0xe4ec0d1edeadbeef` with no new trap.
6. Prints a `VERDICT` line (trap counts, scause, stval, sepc,
   canary readback, FNV-1a checksum over the measured values) and
   `checks=N fails=0`, then `RESULT: PASS` only if every check held.

Measured on QEMU 8.2.2: phase A faults with `scause = 0xd`,
`stval = 0x40000000`; phase B reads back the canary with 0 new
traps; identical across 3 runs. This behavior is what QEMU 8.2.2
exhibits; it is the emulator's implementation of the MXR rule, not a
measurement from silicon.

## Files

- `mxr_main.c`: M-mode setup, Sv39 table build, S-mode test phases,
  UART reporting, PASS/FAIL verdict.
- `mxr_trap.S`: S-mode trap entry (full register save, dedicated trap
  stack, C handler records the one expected load page fault and skips
  the faulting `ld`) and the parking M-mode vector.
- `mxr.h`: bit and address definitions shared by the two files.
- `PROOF.md`: the verification record with the genuine build log
  and all three raw QEMU run outputs.
- `bench-logs/`: `build.log`, `run1.log`, `run2.log`, `run3.log`.

## Build and run

```
make sstatus-mxr.elf
make run-sstatus-mxr
```
