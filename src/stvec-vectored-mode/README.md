# stvec vectored mode: exceptions at BASE, interrupts at BASE+4*cause

Proves the true vectored-mode entry rule: in stvec MODE=1, synchronous
exceptions always enter at BASE and only interrupts enter at
BASE+4*cause. Installs a vectored stvec over a two-slot table of
single 4-byte jal stubs, delegates only the illegal-instruction
exception (medeleg bit 2) and only the supervisor software interrupt
(mideleg bit 1), then measures both paths.

## What it does

1. Boots in M-mode, installs a direct-mode mtvec/mscratch pair (the
   M-mode handler only records; no M-mode trap is expected) and
   records the boot medeleg/mideleg values.
2. Programs selective delegation with readback checks: medeleg keeps
   exactly bit 2, mideleg keeps the zero-write forced set plus
   exactly bit 1 (bit 9 stays clear).
3. Installs stvec MODE=1 over the two-slot jal table, reads it back,
   and requires the mode bits to read 1 and BASE to equal the table
   address. Opens the address space with one PMP NAPOT entry, clears
   mie and mstatus.MIE, then drops to S-mode.
4. Phase A: executes the 4-byte illegal word 0xffffffff at a labeled
   site. The trap must enter at the slot-0 stub (BASE), recorded as
   the hardware entry pc, with scause = 0x2 and sepc exactly at the
   site. The handler skips the word by advancing sepc by 4 and raises
   the done flag.
5. Phase B: pends mip.SSIP from S-mode with SIE off, then sets SIE.
   The pending interrupt must trap exactly once at the slot-1 stub
   (BASE+4) with scause = 0x8000000000000001 and sepc at the
   interrupted nop; the handler clears SSIP so the level-triggered
   source fires once.
6. A no-pending control and a quiet window prove no extra traps fire;
   the M-mode trap count must stay zero throughout.

## Correction of the backlog premise

The backlog item as written claimed an illegal instruction would land
at BASE+8 and an ecall at BASE+4*cause, as if exceptions vectored by
cause like interrupts. The privileged spec (Trap Vector Base Address
Register) says the opposite: in vectored mode synchronous exceptions
enter at BASE and only interrupts enter at BASE+4*cause. This module
implements and proves the true rule, not the written premise. The
full correction is documented in PROOF.md.

## Files

- `stvm_trap.S`: the two-slot vector table (each stub is `jal t0,
  stvm_s_entry`, so the recorder recovers the exact hardware entry pc
  as t0 - 4), the common S-mode recorder (trap count, scause, sepc,
  sip at entry, entry pc, done flag; skips the illegal word on the
  exception path, clears SSIP on the interrupt path), and the M-mode
  recorder.
- `stvm_main.c`: M-mode setup, delegation readbacks, stvec install
  with mode/BASE readback, the S-mode drop, both phases, the
  controls, and 22 checks.
- `bench-logs/`: `build.log` and three QEMU run logs.
