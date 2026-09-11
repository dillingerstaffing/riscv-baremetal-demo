# sie.SSIE gate test (backlog item "riscv sie-ssip-clear-suppresses")

`sie.SSIE` is the S-mode enable bit for the supervisor software
interrupt, independent of the global `sstatus.SIE` gate. M-mode
delegates the interrupt via `mideleg` bit 1, pends `mip.SSIP`, and
drops to S-mode with `SIE` set and `SSIE` clear: the pending bit must
stay pending with no trap, and setting `SSIE` must deliver exactly
one S-mode trap (`scause = 0x8000000000000001`) with no re-delivery.

## What it does

1. Boots in M-mode, installs minimal M-mode and S-mode trap handlers
   (direct-mode `mtvec`/`mscratch` and `stvec`/`sscratch` scratch
   areas). The M-mode handler only records `mcause`/`mepc` and parks;
   the S-mode handler records `scause`/`sepc`/the `sip` value at
   entry, clears `sip.SSIP`, and raises a done flag.
2. Writes bit 1 to `mideleg` and requires the readback to show the
   bit set with bit 9 clear (only the SSI is delegated). `medeleg`
   is written zero and read back zero.
3. Opens the whole address space to S-mode with one PMP NAPOT entry,
   grants `rdcycle`/`rdtime` to lower modes via `mcounteren`, clears
   `mie` and `mstatus.MIE`, pends `SSIP` from M-mode with `csrs mip`
   (S-mode `sip` writes to bit 1 are dropped on this hart, so the
   pend happens before the drop), clears `sie` entirely, and drops
   to S-mode via `sret` with `sstatus.SIE` set.
4. Phase 1: polls a bounded window of 2,000,000 iterations with SIE
   = 1 and SSIE = 0. `sip.SSIP` must read 1 on every iteration while
   the trap count stays 0.
5. Phase 2: sets `sie.SSIE` inside a labeled wait loop. Exactly one
   trap must fire, at an instruction inside the loop, with
   `scause = 0x8000000000000001`; the handler records `scause`,
   `sepc`, and the `sip` value at entry, then clears `SSIP`.
6. Phase 3: polls a bounded quiet window with SIE and SSIE both on
   and the source cleared. The trap count must stay 1 and
   `sip.SSIP` must read 0: no re-delivery.

## Run

`make run-sie-ssip-clear-suppresses` (or `qemu-system-riscv64 -machine
virt -nographic -bios none -kernel sie-ssip-clear-suppresses.elf`).

Three runs on QEMU 8.2.2 produce byte-identical output; see
`bench-logs/`. The verdict values are summarized in `PROOF.md`.
