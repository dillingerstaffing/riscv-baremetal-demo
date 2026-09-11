<!-- PROOF-HEADER
Checks: 16
Mismatches: 0
Checksum: 0x8bd6d3fe55b327cb
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: stimecmp one-shot disarm with quiet window (S-mode)

Backlog item 137. One supervisor timer interrupt is armed 1000 mtime ticks
ahead of the CLINT mtime (Sstc extension, stimecmp CSR). M-mode boot probes
Sstc via menvcfg.STCE, delegates the supervisor timer interrupt to S-mode
via mideleg bit 5, disarms stimecmp once, and drops to S-mode with mret.
The S-mode handler takes the trap, disarms the timer by writing all-ones
to stimecmp while still inside the handler, and returns. After that single
trap, with sstatus.SIE and the sie STIE bit still enabled, the hart spins
through a quiet window of 1,000,000 rdcycle reads; zero re-delivery traps
may fire.

This is the S-mode analog of the shipped src/mtimecmp-oneshot module
(item 148): that module proves the same one-shot property for the machine
timer interrupt (mtimecmp, M-mode); this module proves it for the
supervisor timer interrupt (stimecmp, S-mode) with mideleg delegation.
Boot, trap entry, handler, and reporting code are written separately; the
two modules share only boot.S and the UART driver via the Makefile.

## What the hardware guarantees, and what is measured

- The fundamental truth: a supervisor timer interrupt is a one-shot event
  once stimecmp is disarmed inside the S-mode handler. The pending state
  (sip.STIP) clears and no new trap fires until a future stimecmp is armed.
- M-mode performs exactly one stimecmp write, the boot disarm. S-mode
  performs exactly two: the arm write and the in-handler disarm write.
  Write counters instrumented at the write sites must read 1 and 2 at the
  end; a final stimecmp readback of all-ones after the quiet window proves
  nothing re-armed behind the handler's back.
- The sip CSR's STIP bit (bit 5) is read inside the handler after the
  disarm write: it must read clear, a hardware-level confirmation that
  the pending timer interrupt was withdrawn.
- The C trap handler increments a trap counter on every trap, expected
  or not, so any re-delivery during the quiet window is caught and fails
  the run. sie has only STIE set, so no other interrupt source is
  enabled; medeleg is left at zero and an M-mode vector reports any
  unexpected M-mode trap instead of hanging silently.

## Measured results (3 runs, QEMU 8.2.2 `virt`, single hart, S-mode)

Run logs: bench-logs/run-1.txt, bench-logs/run-2.txt, bench-logs/run-3.txt.
Build log: bench-logs/build.log.

All three runs: RESULT: PASS, checks=16, fails=0, qemu exit code 0
(finisher device 0x100000 = 0x5555).

| Run | trap_count | scause | sepc | quiet reads | quiet traps | stimecmp_final | m_writes | s_writes | checksum |
|-----|-----------|--------|------|-------------|-------------|----------------|----------|----------|----------|
| 1 | 1 | 0x8000000000000005 | 0x80000342 | 1000000 | 0 | 0xffffffffffffffff | 1 | 2 | 0x8bd6d3fe55b327cb |
| 2 | 1 | 0x8000000000000005 | 0x80000342 | 1000000 | 0 | 0xffffffffffffffff | 1 | 2 | 0x8bd6d3fe55b327cb |
| 3 | 1 | 0x8000000000000005 | 0x80000342 | 1000000 | 0 | 0xffffffffffffffff | 1 | 2 | 0x8bd6d3fe55b327cb |

The checksum is FNV-1a 64-bit over the deterministic measured values, in
order: trap_count, scause, sepc, quiet_window_traps, the post-quiet-window
stimecmp readback, m_writes, s_writes, quiet_reads_done, each hashed as one
64-bit word. It is identical across all three runs.

The three log files are NOT byte-identical, and that is honest: two lines
carry host-timing-dependent values. The armed stimecmp value differs per
run (0xa750, 0x10963, 0x1756d) because the CLINT mtime at boot depends on
host scheduling, and the quiet-window rdcycle delta differs per run
(106299090, 115842675, 104922390) for the same reason. Every
verdict-relevant line (trap count, scause, sepc, quiet traps, write
counters, checksum, RESULT) is identical across the three runs.

Run 3 additionally exercised the already-pending path: its clean-arm
statistic read 0, meaning the host was descheduled for more than 100 us
between the arming mtime read and the verify read, so the timer was
already pending when SIE was enabled. The trap still fired exactly once
with scause 0x8000000000000005, the handler disarmed it, and the quiet
window stayed clean. The clean-arm value is a recorded statistic, not a
gating check, precisely so this host jitter is documented rather than
assumed away.

Setup facts confirmed on this emulator: the menvcfg.STCE probe stuck
(Sstc present), mideleg read back 0x1464 (bit 5, the written STI
delegation bit, set; bits 2, 6, 10, 12 are read-only-1 on QEMU's
mideleg as documented by the mideleg-route module), stimecmp read
all-ones at S-mode entry (the M-mode boot disarm is visible), and sip
read 0 before arming. The first trap's sepc (0x80000342) equals the
stos_loop label address, inside the arming spin loop.

## Build

`make stimecmp-one-shot.elf CROSS=/home/hatch/workspace/toolchains/compat-bin/riscv64-unknown-elf-`
(xPack GNU RISC-V Embedded GCC 15.2.0), clean build, no warnings; the only
linker note is the standard RWX LOAD-segment warning shared by every
module in this repo. Runs used
`/home/hatch/workspace/qemu/usr/bin/qemu-system-riscv64` (QEMU emulator
version 8.2.2) with
`LD_LIBRARY_PATH=/home/hatch/workspace/qemu/usr/lib/x86_64-linux-gnu:/home/hatch/workspace/qemu/lib/x86_64-linux-gnu`,
invoked as `make run-stimecmp-one-shot QEMU=<that binary>` under
`timeout 60`.

## Limits, stated honestly

- This is emulator behavior, not silicon: the one-shot property was
  verified against QEMU 8.2.2's CLINT/stimecmp model on the `virt`
  machine, single hart. Real silicon implements the same privileged-ISA
  contract (stimecmp compare, sip.STIP withdrawal on disarm), but this
  run measured the emulator.
- The "M-mode does not re-arm" claim covers the code in this module:
  every stimecmp write site is counter-instrumented (1 M-mode, 2 S-mode)
  and the final readback is all-ones. There is no other software on the
  hart that could write it.
- The quiet window is 1,000,000 rdcycle reads (on the order of 10^8
  cycles on this emulator), not an unbounded proof; it bounds the claim
  to "no re-delivery within the window," which is what was measured.
