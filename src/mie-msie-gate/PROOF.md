<!-- PROOF-HEADER
Checks: 20
Mismatches: 0
Checksum: 0x908c0c90d15478ee
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: mie.MSIE enable-gate for the machine software interrupt

mie.MSIE (bit 3) is the M-mode enable bit for the machine software
interrupt, independent of the pending bit. In M-mode on QEMU, with
mstatus.MIE set and the CLINT msip pended for hart 0 while MSIE is
clear, mip.MSIP reads pending with zero traps over a bounded window;
setting MSIE then delivers exactly one trap with
mcause = 0x8000000000000003. This is the enable-gate complement to
the shipped src/mip-pending-no-trap (which measured the pending
reflection with the enable off) and src/mie-stie (the analogous
gate for the timer interrupt).

## What the hardware guarantees, and what is measured

- The RISC-V privileged spec's trap logic takes interrupt i when
  mstatus.MIE is set and both mie[i] and mip[i] are set. MSIE is
  bit 3 of mie and MSIP is bit 3 of mip. With mie = 0x0, a
  pending MSIP must not trap, even with MIE set; setting mie bit
  3 while MSIP is still pending must deliver the interrupt at the
  next instruction boundary.
- The msip bit is pended by a 32-bit MMIO write of 1 to the
  CLINT msip register for hart 0 at 0x02000000 on the virt board;
  the MMIO readback (1) and the mip bit-3 readback (pending) are
  the independent confirmations that the pend took effect while
  no trap fired.
- The handler clears the CLINT msip with a 32-bit MMIO write of 0
  inside the trap (level-triggered source: clearing first is what
  stops the re-fire), records mcause/mepc, and bumps the trap
  counter; a further quiet window must show zero re-deliveries.
- The machine timer is disarmed at startup (mtimecmp = all-ones)
  and no S-mode source is programmed, so the machine software
  interrupt is the only source that can ever be taken; the
  mcause value plus the trap counts are the ground truth for the
  gate. MIE stays set for the whole run, so mie.MSIE is the only
  gating variable.

## Measured results (3 runs, QEMU 8.2.2 `virt`, M-mode, single hart)

Run logs: bench-logs/run1.log, bench-logs/run2.log, bench-logs/run3.log.
Build log: bench-logs/build.log.

The three run transcripts are byte-identical (md5
a5dffd3098964c100bddb6124e12c417 for all three); every printed
verdict line below is identical across the 3 runs:

- boot: mie=0x0, mstatus.MIE=0, mip=0x0, msip-mmio=0;
  mtvec=0x800001e4 (direct mode); mstatus.MIE=1 after set.
- Phase A (MSIE clear): mie readback=0x0; msip-mmio readback=1;
  mip=0x8 (MSIP SET); quiet window (2,000,000 rdcycle deltas):
  traps=0, mip-after-window=0x8 (MSIP still SET).
- Phase B (control): mie readback=0x8; mcause=0x8000000000000003,
  mepc=0x800004dc, trap-count=1; mip after handler=0x0 (MSIP
  clear).
- Phase C (quiet window): traps-after-quiet=1.
- End: mstatus.MIE=1, mie=0x8. RESULT: PASS (traps=1), QEMU exit 0.

FNV-1a over the run transcript: 0x908c0c90d15478ee, identical for
all 3 runs. 20 checks, 0 failures, QEMU exit 0 on every run.

## Verdict logic

PASS requires all of: mie reads 0x0, mstatus.MIE reads 0, mip MSIP
reads clear, and msip MMIO reads 0 at boot; the trap vector takes
the handler address in direct mode; mstatus.MIE reads 1 after the
set and stays 1; phase-A mie readback is exactly 0x0; the msip
MMIO readback is 1 after the pend; mip MSIP reads pending after
the pend; the phase-A quiet window fires zero traps while mip
still reads MSIP pending (0x8); phase-B mie readback is exactly
0x8; exactly 1 trap fires with mcause 0x8000000000000003; the
handler's msip clear leaves mip MSIP reading clear (0x0); the
phase-C quiet window shows zero additional traps; mie still reads
0x8 at the end.

## Limits, stated honestly

- This runs on QEMU 8.2.2's virt machine, an emulator, not on
  silicon. The gate itself (pending + MIE + mie bit pattern ->
  trap or silence) is the emulated CSR logic, which is what the
  module measures.
- The phase-A window is bounded by 2,000,000 rdcycle deltas, not
  by wall-clock time; on this emulator that is far more than the
  instruction boundary at which an enabled pending interrupt
  fires, so a silent window is the mechanism, not a slow setup.
  The host-visible duration of the window varies with the host,
  which is why the window length is never printed: the verdict
  lines (mie/mip readbacks, trap counts, mcause, mepc) are the
  byte-identical content.
- mepc=0x800004dc on all 3 runs is the resume address in the
  phase-B enable sequence of this exact binary; it is published
  as a measurement, not as a claim about any other build.
- The msip access uses the 32-bit MMIO form at 0x02000000. The
  64-bit fault noted in src/msip/ applies to 64-bit accesses on
  this register, which this module never issues.
