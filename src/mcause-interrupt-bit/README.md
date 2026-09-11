# mcause interrupt-bit check

Tests the rule that `mcause` bit 63 distinguishes an asynchronous
interrupt (set) from a synchronous exception (clear), with two
deliberate M-mode traps on a bare-metal hart (no OpenSBI).

## What it does

1. Installs a minimal M-mode trap handler (direct-mode `mtvec`,
   `mscratch` scratch area) that records `mcause`/`mepc`, bumps a
   per-class trap counter, and resumes: past the 4-byte `ecall` for
   the exception, in place for anything else.
2. Control: a quiet window with no source armed must produce zero
   traps.
3. Phase 1 (exception): executes an M-mode `ecall` at a labeled site
   and requires the handler-recorded `mcause` to read `0xb`, bit 63
   to read 0, and the recorded `mepc` to equal the ecall instruction
   address.
4. Phase 2 (interrupt): arms the CLINT `mtimecmp` 50000 mtime ticks
   (5 ms) ahead with a bounded re-arm retry, spins until the machine
   timer interrupt fires, and requires the handler-recorded `mcause`
   to read `0x8000000000000007` and bit 63 to read 1. The handler
   disarms `mtimecmp` to all-ones; the run requires the disarm
   readback and proves no re-delivery in a further quiet window.
5. Requires zero unexpected traps across the whole run, then
   publishes `checks`, `mismatches`, and an FNV-1a checksum over the
   recorded values (deterministic fields only), plus the PASS/FAIL
   verdict.

Measured finding: on this hart (QEMU 8.2.2, virt machine) the M-mode
ecall reports `mcause = 0xb` with bit 63 clear and the machine timer
interrupt reports `mcause = 0x8000000000000007` with bit 63 set.

## Files

- `mib_main.c`: the test sequence, UART reporting, checks/mismatch
  counters, the FNV-1a checksum, and the PASS/FAIL verdict.
- `mib_trap.S`: the trap entry.
- `PROOF.md`: the full proof record with the build log and per-run
  measurements.
- `bench-logs/`: the build log, the three raw QEMU run logs, and the
  host CPU info.
