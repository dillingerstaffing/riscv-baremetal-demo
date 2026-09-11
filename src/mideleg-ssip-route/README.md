# mideleg SSIP routing (backlog item "riscv mideleg-ssip-route")

Sets `mideleg` to delegate only the supervisor software interrupt
(bit 1, readback-verified), drops to S-mode, and checks that the
interrupt's own source, `mip.SSIP`, traps to S-mode with
`scause = 0x8000000000000001` while a pending CLINT `msip` does not
take this path.

## What it does

1. Boots in M-mode, installs minimal M-mode and S-mode trap handlers
   (direct-mode `mtvec`/`mscratch` and `stvec`/`sscratch` scratch
   areas). The M-mode handler only records `mcause`/`mepc`; the
   S-mode handler records `scause`/`sepc`/the `sip` value at entry,
   clears `sip.SSIP`, and raises a done flag.
2. Records the boot `mideleg` value, writes zero to `mideleg` and
   reads back the forced set, then writes bit 1 and requires the
   readback to equal the forced set with only bit 1 added (the write
   changed nothing else). Bit 9 must read back clear, so the
   delegation is selective to the supervisor software interrupt.
   `medeleg` is written zero and read back zero.
3. Opens the whole address space to S-mode with one PMP NAPOT entry,
   clears `mie` and `mstatus.MIE`, clears `msip` and `SSIP`, and drops
   to S-mode via `sret`.
4. Control: with `sie.SSIE` and `sstatus.SIE` set and nothing pending,
   polls and requires 0 traps of either kind.
5. Negative control: sets the CLINT `msip` for hart 0 (32-bit access;
   this QEMU's CLINT faults on 64-bit `msip` accesses), reads back 1,
   polls with `SIE` on while it stays pending, and requires 0 traps
   of either kind: the CLINT drives `mip.MSIP` (cause 3, the machine
   software interrupt), which `mideleg` bit 1 does not delegate. Then
   clears `msip` and reads back 0.
6. Pends `SSIP` with `csrs sip` while `SIE` is off (stable readback,
   must read `0x2`), then sets `sstatus.SIE`. The pending interrupt
   is taken at the next instruction boundary; the program requires
   exactly one S-mode trap with `scause = 0x8000000000000001`,
   `sepc` equal to the address of the interrupted instruction
   (captured with an in-assembly local label), `sip` showing `SSIP`
   at handler entry and `0x0` after the handler cleared it, and
   0 M-mode traps.
7. Quiet window: polls with `SIE` on and requires the trap counts
   unchanged; prints `RESULT: PASS` only if all 17 checks held.

## Why SSIP and not the CLINT msip

The backlog item as written names the CLINT `msip` as the interrupt
source. A pre-build experiment on QEMU 8.2.2 measured that a CLINT
`msip` set drives `mip.MSIP` (bit 3, the machine software interrupt,
cause 3): with `mie.MSIE` and `mstatus.MIE` armed it traps to M-mode
with `mcause = 0x8000000000000003` and zero S-mode traps, because
`mideleg` bit 1 covers cause 1, not cause 3. The supervisor software
interrupt's own source is `mip.SSIP` (bit 1); that is what the module
pends, and the negative control above re-verifies the non-routing
inside the run.
