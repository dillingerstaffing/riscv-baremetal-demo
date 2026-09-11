<!-- PROOF-HEADER
Checks: 20
Mismatches: 0
Checksum: 0x5409721809eefeb2
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: mie.MTIE enable-gate for the machine timer interrupt

mie.MTIE (bit 7) is the M-mode enable bit for the machine timer
interrupt, independent of the pending bit. In M-mode on QEMU, with
mstatus.MIE set and the CLINT timer armed so mip.MTIP goes pending
while MTIE is clear, mip.MTIP reads pending with zero traps over a
bounded window; setting MTIE then delivers exactly one trap with
mcause = 0x8000000000000007. This is the enable-gate complement to
the shipped src/mip-pending-no-trap (which measured the pending
reflection with the enable off) and src/mie-msie-gate (the analogous
gate for the software interrupt).

## What the hardware guarantees, and what is measured

- The RISC-V privileged spec's trap logic takes interrupt i when
  mstatus.MIE is set and both mie[i] and mip[i] are set. MTIE is
  bit 7 of mie and MTIP is bit 7 of mip. With mie = 0x0, a
  pending MTIP must not trap, even with MIE set; setting mie bit
  7 while MTIP is still pending must deliver the interrupt at the
  next instruction boundary.
- The timer is armed by a 64-bit MMIO write of
  mtime + 5000 ticks to the CLINT mtimecmp register for hart 0 at
  0x02004000 on the virt board (mtime runs at 10 MHz, so 5000
  ticks is 500 us); a bounded poll waits for the mip bit-7
  readback to go pending before the phase-A window starts, so the
  pend is itself a measured observation, not assumed.
- The handler disarms the timer by writing mtimecmp to all-ones
  inside the trap (a compare above the running mtime clears MTIP
  and is what stops the re-fire), records mcause/mepc, and bumps
  the trap counter; a further quiet window must show zero
  re-deliveries.
- The CLINT msip is never pended and no S-mode source is
  programmed, so the machine timer interrupt is the only source
  that can ever be taken; the mcause value plus the trap counts
  are the ground truth for the gate. MIE stays set for the whole
  run, so mie.MTIE is the only gating variable.

## Measured results (3 runs, QEMU 8.2.2 `virt`, M-mode, single hart)

Run logs: bench-logs/run1.log, bench-logs/run2.log, bench-logs/run3.log.
Build log: bench-logs/build.log.

The three run transcripts are byte-identical (md5
04e6c0ae2df039864273b0ae84649d3d for all three); every printed
verdict line below is identical across the 3 runs:

- boot: mie=0x0, mstatus.MIE=0, mip=0x0,
  mtimecmp=0xffffffffffffffff (disarmed);
  mtvec=0x800001e4 (direct mode); mstatus.MIE=1 after set.
- Phase A (MTIE clear): mie readback=0x0; mip=0x80 (MTIP SET)
  after the bounded pend poll; quiet window (2,000,000 rdcycle
  deltas): traps=0, mip-after-window=0x80 (MTIP still SET).
- Phase B (control): mie readback=0x80;
  mcause=0x8000000000000007, mepc=0x800004fc, trap-count=1;
  mip after handler=0x0 (MTIP clear).
- Phase C (quiet window): traps-after-quiet=1.
- End: mstatus.MIE=1, mie=0x80. RESULT: PASS (traps=1), QEMU
  exit 0 on every run.

FNV-1a over the run transcript: 0x5409721809eefeb2, identical for
all 3 runs. 20 checks, 0 failures, QEMU exit 0 on every run.

## Verdict logic

PASS requires all of: mie reads 0x0, mstatus.MIE reads 0, mip
MTIP reads clear, and mtimecmp reads all-ones at boot; the trap
vector takes the handler address in direct mode; mstatus.MIE
reads 1 after the set and stays 1; phase-A mie readback is
exactly 0x0; mip MTIP reads pending after the bounded pend poll;
the phase-A quiet window fires zero traps while mip still reads
MTIP pending (0x80); phase-B mie readback is exactly 0x80;
exactly 1 trap fires with mcause 0x8000000000000007; the
handler's mtimecmp disarm leaves mip MTIP reading clear (0x0);
the phase-C quiet window shows zero additional traps; mie still
reads 0x80 at the end.

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
- mepc=0x800004fc on all 3 runs is the resume address in the
  phase-B enable sequence of this exact binary; it is published
  as a measurement, not as a claim about any other build.
- The pend poll is bounded by 10,000,000 spin iterations; a
  failure there would be a FAIL, so the observed SET readback is
  a measurement, not an assumption.
