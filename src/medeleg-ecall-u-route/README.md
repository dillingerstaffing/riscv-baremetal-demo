# medeleg bit-8 U-mode ecall route (backlog item "riscv medeleg-ecall-u-route")

Issues one `ecall` from U-mode with `medeleg` bit 8 set and shows
the trap landing in S-mode (`scause = 8`), with no M-mode trap
firing at all. A restore `ecall` from S-mode afterwards (bit 9
clear) is taken in M-mode, whose handler writes the boot `medeleg`
value back. Per-mode trap counts, cause codes, and `sepc`/`mepc`
against the captured ecall addresses are published over UART.

## What it does

1. Boots in M-mode, installs minimal M-mode and S-mode trap
   handlers (direct-mode `mtvec`/`mscratch` and
   `stvec`/`sscratch` save areas). The M-mode handler records
   `mcause`/`mepc`/`mstatus`; if the restore is armed it writes
   the boot `medeleg` back, records the readback, and `mret`s to
   the finalizer with `mstatus.MPP` set to M-mode. If an M-mode
   trap fires before the restore is armed, the U-mode ecall was
   not delegated: the handler raises a premature flag and resumes
   at the finalizer, which reports the evidence and fails instead
   of parking silently. The S-mode handler records
   `scause`/`sepc`/`sstatus`, raises a done flag, and redirects
   to the S-mode reporter (`sepc` = reporter, `sstatus.SPP` = 1)
   instead of resuming the payload.
2. Records the boot `medeleg`, writes `0x100`, and requires the
   readback to equal `0x100` (bit 8 verified set, nothing else
   set).
3. Opens the whole address space to lower modes with one PMP
   NAPOT entry (lower modes default-deny; the U-mode payload must
   fetch its ecall), clears `mie` and `mstatus.MIE` (no interrupt
   of either kind can fire), and drops to U-mode via `sret` with
   `sstatus.SPP` = 0.
4. The U-mode payload stores the ecall address (taken with an
   in-assembly label before the ecall executes, since the handler
   never resumes this payload) and issues `ecall`. Delegated by
   bit 8, the trap lands in the S-mode handler, which records it
   and redirects to the S-mode reporter.
5. The reporter prints every measured value, runs 7 delegation
   checks (readback exactly `0x100`, exactly one S-mode trap,
   `scause` = 8, `sepc` at the ecall, `sstatus.SPP` = 0 showing
   arrival from U-mode, zero M-mode traps), takes a quiet window
   with unchanged counts, arms the restore, stores the
   restore-ecall address with an in-assembly store before the
   ecall executes, and issues `ecall` from S-mode.
6. The M-mode handler writes the boot `medeleg` back, records the
   readback, and resumes at the finalizer, which runs 3 restore
   checks (`mcause` = 9, `mepc` at the restore ecall, `medeleg`
   restored to the boot value), prints an FNV-1a checksum over the
   verdict values, and prints `RESULT: PASS` only if all 10 checks
   held.
