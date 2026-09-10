# PROOF: mtimecmp one-shot disarm with quiet window

Backlog item 148. One machine timer interrupt is armed exactly one
mtime tick ahead of the CLINT mtime (10 MHz timebase on the QEMU virt
board, so one tick is 100 ns). The M-mode handler takes the trap,
disarms the timer by writing all-ones to mtimecmp while still inside
the handler, and returns. After that single trap, with mstatus MIE and
the mie MTIE bit still enabled, the hart spins through a quiet window
of 1,000,000 rdcycle reads; zero re-delivery traps may fire.

This is the disarm path of the shipped src/mtimecmp module, isolated:
that module measures delivery accuracy over 1000 armed trials; this
module measures only what happens after the disarm write, and shares
no trial or reporting code with it.

## What the hardware guarantees, and what is measured

- The CLINT raises the machine timer interrupt when mtime >= mtimecmp
  and the hart takes it when mstatus.MIE and mie.MTIE are both set.
  Writing all-ones to mtimecmp while in the handler removes the only
  pending source before mret.
- The mip CSR's MTIP bit (bit 7) is read inside the handler after the
  disarm write: it must read clear, a hardware-level confirmation
  that the pending timer interrupt was withdrawn.
- The C trap handler increments a trap counter on every trap, expected
  or not, so any re-delivery during the quiet window is caught and
  fails the run. mie has only MTIE set, so no other interrupt source
  is enabled.

## Measured results (3 runs, QEMU 8.2.2 `virt`, M-mode, single hart)

Run logs: bench-logs/run-1.txt, bench-logs/run-2.txt, bench-logs/run-3.txt.
Build log: bench-logs/build.log.

@@NUMBERS@@

Per run (raw logs verbatim in bench-logs/run-N.txt):

- Run 1: arm mtimecmp=0x6c30 (mtime+1), clean=0; trap_count=1,
  mcause=0x8000000000000007, mepc=0x80000332 (== os_loop, inside the
  spin loop [0x80000332, 0x8000033a)); mip after disarm=0x0 (MTIP
  clear); quiet window 1,000,000 rdcycle reads, rdcycle
  delta=101318910, traps in window=0; RESULT: PASS.
- Run 2: arm mtimecmp=0x255f7 (mtime+1), clean=0; trap_count=1,
  mcause=0x8000000000000007, mepc=0x80000332; mip after
  disarm=0x0 (MTIP clear); quiet window 1,000,000 reads, rdcycle
  delta=142814880, traps in window=0; RESULT: PASS.
- Run 3: arm mtimecmp=0xb684 (mtime+1), clean=0; trap_count=1,
  mcause=0x8000000000000007, mepc=0x80000332; mip after
  disarm=0x0 (MTIP clear); quiet window 1,000,000 reads, rdcycle
  delta=176866725, traps in window=0; RESULT: PASS.

The VERDICT lines (trap_count, mcause, mepc, quiet window length,
quiet window traps) are byte-identical across the 3 runs; only the
arm mtimecmp value and the quiet-window rdcycle delta (both host
timing, not the mechanism) differ.

## Verdict logic

PASS requires all of: exactly 1 trap fired; its mcause is the machine
timer interrupt (0x8000000000000007); its mepc lies inside the trial
spin loop between the os_loop and os_done labels; the mip MTIP bit
reads clear after the disarm write; and the quiet window shows zero
additional traps. The printed VERDICT lines (trap_count, mcause, mepc,
quiet window length, quiet window traps) are byte-identical across
the 3 runs.

## Limits, stated honestly

- This runs on QEMU 8.2.2's virt machine, an emulator, not on silicon.
  The emulated CLINT's mtime tracks the host's virtual timebase, so
  exact trap-delivery timing reflects host scheduling, not a hardware
  timer's phase relationship to the pipeline.
- One mtime tick is 100 ns at the 10 MHz timebase, while the arm
  sequence (an mtime read, the mtimecmp write, and the verify read)
  spans more than one tick on this emulator, so the interrupt is
  already pending when MIE is enabled (clean=0 on all 3 runs). The
  arm write itself is exactly mtime+1 as specified, and the
  disarm-path verification (the one trap, the all-ones write inside
  the handler, zero re-deliveries across the 1,000,000-read quiet
  window) is fully exercised either way. The clean-arm statistic is
  published rather than hidden.
- The disarm write itself (mtimecmp = all-ones in the handler) and the
  resulting absence of re-delivery are the register writes and the
  measured trap counts this module's claims rest on.
