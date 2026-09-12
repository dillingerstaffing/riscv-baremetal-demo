# fflags-nx-only

Checks that `fflags.NX` is the accrued inexact flag, in M-mode
under QEMU 8.2.2 on the virt machine.

Phase A runs one volatile in-asm `fadd.d` with rm=DYN on the
operand pair (1e16, 1.0), loaded as bit patterns with
`fmv.d.x`: the exact sum 10^16 + 1 is not representable (ulp 2
in the binade, the exact sum halfway between the even- and
odd-significand neighbors), so the run requires `fcsr` to read
exactly `0x01` afterward (NX set, NV/DZ/OF/UF clear, frm still
RNE) and the result bits to equal the correctly rounded value
`0x4341c37937e08000` (RNE ties-to-even picks the even
significand, i.e. 1e16 itself). Phase B is the exact control:
`fadd.d(1.0, 2.0)` must deliver `0x4008000000000000` (3.0) with
`fcsr` reading exactly `0x00` (no accrued flag moved). Each
phase writes `csrw fcsr, 0x00` first and requires the exact
0x00 readback; `fcsr` restores to its exact boot value at the
end. `mstatus.FS` is set to Initial before any FP use, a
counting M-mode trap handler is installed as a safety net (the
run requires its counter to stay 0), and a final FS readback
requires FS != Off after the FP writes.

Files:

- `fnxo_main.c`: two-phase driver, UART reporting, checks,
  checksum, PASS/FAIL verdict with virt test-device finisher
  shutdown on PASS and a parked hart on FAIL.
- `fnxo_trap.S`: counting M-mode trap entry (record, count,
  resume).
- `PROOF.md`: proof log with the machine-readable header, the
  measured values, and the verification record.
- `bench-logs/`: build log and the three raw QEMU run logs.

Build and run from the repo root (CROSS to taste):

    make fflags-nx-only.elf
    make run-fflags-nx-only

On a passing run QEMU exits 0 after `RESULT: PASS (checks=18)`.
