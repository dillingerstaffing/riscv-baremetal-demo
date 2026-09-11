# scause INTERRUPT-bit probe (backlog item 125)

Delegates a supervisor timer interrupt (`mideleg` bit 5) and the load
page fault exception (`medeleg` bit 13) to S-mode, then triggers both
in S-mode and records `scause` for each: the fault must report bit 63
clear with code 13, the timer interrupt bit 63 set with code 5.

## What it does

1. Boots in M-mode on QEMU 8.2.2 (`virt`), installs a minimal M-mode
   trap vector that parks the hart on any trap (none should reach
   M-mode), and probes Sstc (`menvcfg.STCE`, required: S-mode arms
   `stimecmp` directly).
2. Opens the whole address space to S-mode with one PMP NAPOT R/W/X
   entry, grants S-mode `rdtime` via `mcounteren`, and builds an Sv39
   table by hand: a 1 GiB megapage identity-mapping
   `[0x80000000, 0xC0000000)` (code, data, stack, tables), a 4 KiB
   UART page at `0x10000000`, and every other entry invalid, so VA
   `0x40000000` (VPN[2]=1, `root_pt[1]` invalid) is deliberately
   unmapped and the walk dies at the root lookup.
3. Writes `medeleg` = bit 13 (read back `0x2000`, exact) and
   `mideleg` = bit 5. QEMU ORs the hypervisor interrupt bits into
   `mideleg` after every write (observed forced set `0x1444`, the
   same value `src/mideleg-route` measured), so the zero-write
   readback is taken first and the bit-5 write must add exactly that
   bit (`0x1464`).
4. Installs a direct-mode `stvec` S-mode handler, enables Sv39 via
   `satp` (read back MODE=8), and drops to S-mode via `sret`.
5. Fault phase (SIE clear, so exceptions cannot be confused with the
   timer): a 4-byte `ld` from `0x40000000`. The handler records
   `scause`/`stval`, advances `sepc` by 4, and resumes. Requires
   `scause = 13` (bit 63 clear, load page fault),
   `stval = 0x40000000`, and `sepc` equal to the faulting instruction
   address (checked against an in-assembly label).
6. Timer phase: `stimecmp = time + 10000` ticks, `sie.STIE` set, then
   SIE enabled. The handler records `scause`, disarms the source
   (`stimecmp` = all-ones, read back to verify), and raises a done
   flag. Requires `scause = 0x8000000000000005` (bit 63 set, cause 5).
7. Prints both `scause` values and `RESULT: PASS` only if all 18
   checks held: the two `scause` values, `stval`, `sepc`, the disarm
   readback, exactly two S-mode traps, zero unexpected traps, and
   zero M-mode traps.

Measured on QEMU 8.2.2: fault `scause = 0xd`, `stval = 0x40000000`;
timer `scause = 0x8000000000000005`; identical across 3 runs.

## Files

- `scb_main.c`: M-mode setup, Sv39 table build, S-mode test phases,
  UART reporting, PASS/FAIL verdict.
- `scb_trap.S`: S-mode trap handler (`scause` dispatch, sticky fault
  and timer records, `stimecmp` disarm) and the parking M-mode
  vector.
- `PROOF.md`: the verification record with the genuine build log
  and all three raw QEMU run outputs.
- `bench-logs/`: `build.log`, `run1.log`, `run2.log`, `run3.log`.

## Build and run

```
make scause-bit.elf
make run-scause-bit
```
