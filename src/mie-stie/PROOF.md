<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: mie.STIE vs mie.MTIE enable-bit separation

Backlog item 149. In M-mode on QEMU, with mstatus.MIE set and a
machine timer interrupt pending in mip.MTIP, mie = 0x20 (STIE, the
supervisor timer interrupt enable, bit 5) delivers zero traps,
because the pending source's own enable bit (MTIE, bit 7) is clear.
The control then sets mie = 0xA0 (MTIE|STIE): the still-pending
MTIP delivers exactly one machine timer interrupt, proving the
phase-1 silence was the enable bit and not a broken setup. This is
the enable-bit separation complement to the shipped src/mie-msip
(MSIE gate) and src/mie-global (MIE gate) modules.

## What the hardware guarantees, and what is measured

- The RISC-V privileged spec's trap logic takes interrupt i when
  mstatus.MIE is set and both mie[i] and mip[i] are set. With
  mie = 0x20 only bit 5 (STIE) is enabled; the pending bit is
  mip bit 7 (MTIP), whose enable is mie bit 7 (MTIE). A pending
  MTIP with MTIE clear must not trap, even with MIE set.
- The mip CSR's MTIP bit (bit 7) is read back in phase 1 after the
  quiet window: it must still read pending (0x80), a hardware-level
  confirmation that the interrupt sat pending, globally enabled,
  yet undelivered.
- The control enables MTIE while MTIP is still pending; delivery
  must follow immediately (the interrupt is level-pending from the
  CLINT). The handler disarms the timer by writing all-ones to
  mtimecmp while inside the trap (level-triggered source), records
  mcause/mepc, and bumps the trap counter; a further quiet window
  must show zero re-deliveries.
- No S-mode timer source is ever programmed, so the STIE bit
  enables nothing that can fire; the machine timer interrupt is the
  only source that can ever be taken, and the mcause value plus the
  trap counts are the ground truth for the gate. MIE stays set for
  the whole run, so the mie bits are the only gating variables.

## Measured results (3 runs, QEMU 8.2.2 `virt`, M-mode, single hart)

Run logs: bench-logs/run1.log, bench-logs/run2.log, bench-logs/run3.log.
Build log: bench-logs/build.log.

Per run (raw logs verbatim in bench-logs/runN.log):

- Run 1: boot mie=0x0, mstatus.MIE=0, mip=0x0; mtvec=0x800001e4;
  mstatus.MIE=1 after set. Phase 1: mie readback=0x20; MTIP pending
  after 7181 spins, mip=0x80 (MTIP SET); quiet window (2,000,000
  spins): traps=0, mip=0x80 (MTIP still SET). Phase 2 (control):
  mie readback=0xa0; spins-to-trap=0; mcause=0x8000000000000007,
  mepc=0x800004da, trap-count=1; mip after handler=0x0 (MTIP
  clear); quiet window: traps=1. End: mstatus.MIE=1, mie=0xa0.
  RESULT: PASS (traps=1), QEMU exit 0.
- Run 2: identical except MTIP pending after 704 spins;
  mcause=0x8000000000000007, mepc=0x800004da, trap-count=1,
  RESULT: PASS (traps=1), QEMU exit 0.
- Run 3: identical except MTIP pending after 654 spins;
  mcause=0x8000000000000007, mepc=0x800004da, trap-count=1,
  RESULT: PASS (traps=1), QEMU exit 0.

The spins-to-MTIP-pending values (7181/704/654) are host timing
(the CLINT mtime advances on the host's virtual timebase), not the
mechanism; every verdict line (mie/mip readbacks, trap counts,
mcause, mepc) is identical across the 3 runs.

## Verdict logic

PASS requires all of: mie reads 0x0 at boot with mstatus.MIE=0;
mstatus.MIE reads 1 after the set and stays 1; phase-1 mie readback
is exactly 0x20; MTIP reaches pending (mip bit 7 set) within the
poll budget; the phase-1 quiet window fires zero traps while mip
still reads MTIP pending (0x80); phase-2 mie readback is exactly
0xA0; exactly 1 trap fires with mcause 0x8000000000000007; the
handler's disarm clears MTIP (mip reads 0x0); the second quiet
window shows zero additional traps; mie still reads 0xA0 at the
end.

## Limits, stated honestly

- This runs on QEMU 8.2.2's virt machine, an emulator, not on
  silicon. The emulated CLINT's mtime tracks the host's virtual
  timebase, so the spins-to-MTIP-pending values reflect host
  scheduling, not a hardware timer's phase relationship to the
  pipeline. The gate itself (pending + MIE + enable-bit pattern ->
  trap or silence) is the emulated CSR logic, which is what the
  module measures.
- The phase-2 trap lands with spins-to-trap=0 on all 3 runs: the
  interrupt was already pending when MTIE was set, so delivery
  happened on the first instruction boundary after the mie write.
  That immediacy is itself the measurement confirming the pending
  bit was real.
- mepc=0x800004da on all 3 runs is the resume address in the
  phase-2 enable sequence of this exact binary; it is published as
  a measurement, not as a claim about any other build.
- The disarm write (mtimecmp = all-ones in the handler) uses the
  64-bit CLINT access form, which this emulator accepts for the
  64-bit mtimecmp register (the 64-bit fault noted in src/msip/
  applies to the 32-bit msip register, which this module never
  touches).
