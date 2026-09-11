# medeleg bit-9 S-mode ecall destination switch (backlog item "riscv medeleg-ecall-destination")

Runs the same `ecall` from S-mode twice and shows the trap
destination flipping with `medeleg` bit 9: with the bit clear the
trap lands in M-mode (`mcause = 9`), with the bit set it lands in
S-mode (`scause = 9`). Per-mode trap counts for both phases are
published over UART.

## What it does

1. Boots in M-mode, installs minimal M-mode and S-mode trap
   handlers (direct-mode `mtvec`/`mscratch` and
   `stvec`/`sscratch` save areas). The M-mode handler records
   `mcause`/`mepc`/`mstatus`, then loads a continuation address
   from its save area into `mepc` and sets `mstatus.MPP` to M-mode
   before `mret` (`mret` takes the privilege from MPP, and the
   trap arrived from S-mode, so without this the return would
   drop back into S-mode). The S-mode handler records
   `scause`/`sepc`/`sstatus`, advances `sepc` by 4 past the ecall,
   and raises a done flag.
2. Records the boot `medeleg` value, writes 0 and requires the
   readback to be 0 with bit 9 clear.
3. Opens the whole address space to S-mode with one PMP NAPOT
   entry, clears `mie` and `mstatus.MIE` (no interrupt of either
   kind can fire in either phase), arms the phase-2 continuation,
   and drops to S-mode via `sret`.
4. Phase 1: the S-mode payload stores the ecall address (taken
   with an in-assembly label before the ecall executes, since the
   handler never resumes this payload) and issues `ecall`. With
   `medeleg` zeroed the trap lands in M-mode. The handler records
   it and redirects to the phase-2 M-mode setup.
5. Phase 2: writes `0x200` to `medeleg`, requires the readback to
   have bit 9 set, snapshots the phase-1 trap counts, and drops
   to S-mode again. The payload issues `ecall`; delegated by bit
   9, the trap lands in the S-mode handler, which resumes the
   payload after the ecall.
6. The payload prints every measured value, runs 14 checks
   (delegation readbacks, exact trap counts per mode per phase,
   cause codes, `mepc`/`sepc` equal to the captured ecall
   addresses, `mstatus.MPP`/`sstatus.SPP` showing arrival from
   S-mode, no trap leaked into the wrong mode's handler, a quiet
   window with unchanged counts), prints an FNV-1a checksum over
   the verdict values, and prints `RESULT: PASS` only if all 14
   checks held.

## Cause-code note

The backlog item as written names `mcause = 0xb` for the phase-1
trap. `0xb` is the environment call from M-mode; an `ecall`
issued in S-mode traps with cause 9 (environment call from
S-mode) whether or not it is delegated, which is what the
RISC-V privileged specification assigns and what this run
measures on QEMU 8.2.2. The checks assert `0x9`, not `0xb`.
