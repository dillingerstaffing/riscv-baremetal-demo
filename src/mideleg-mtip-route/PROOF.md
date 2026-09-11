<!-- PROOF-HEADER
Checks: 20
Mismatches: 0
Checksum: 0xc02ae00765d9bb85
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: mideleg MTIP-routing check

On QEMU 8.2.2's virt hart, `mideleg` is WARL and bit 7 (the machine
timer interrupt delegation bit) is read-only zero: every write that
sets it reads back with the bit dropped. This module attempts the
delegation three ways, publishes the write/readback triple, then
arms the CLINT machine timer and drops the hart to S-mode to answer
the routing question directly: the armed interrupt lands in M-mode
with `mcause = 0x8000000000000007`, and zero traps reach S-mode.

## Note on the item's premise

The backlog item expected `mideleg` bit 7 to take and the timer
interrupt to route to S-mode. The measurement disproves the
premise: the bit-7 write reads back 0x1444 with bit 7 clear, and the
armed timer traps in M-mode while the hart sits in S-mode. This
module ships the corrected, measured finding, not the expected one.
The verdict logic asserts what was measured. (Precedent: the
shipped src/mideleg-warl module, which published the WARL legalized
readbacks 0x3666/0x1444 and grounded them in QEMU 8.2.2's
`rmw_mideleg64` write path, delegable mask 0x2666, forced bits
0x1444; bit 7 is in neither set.)

## What was measured

M-mode setup (all values read back, not assumed):

- boot: `mideleg`=0x1444.
- `mtvec` takes the M-mode handler address in direct mode;
  `stvec` takes the S-mode handler address in direct mode.
- write 0x0 -> readback 0x1444.
- write 0x80 (bit 7 only) -> readback 0x1444: the bit is dropped.
- write 0x1444|0x80 -> readback 0x1444.
- `mie`=0x80 (MTIE) on readback, `mstatus.MIE` set on readback,
  `mtimecmp` disarmed (all-ones) on readback, `mip.MTIP` clear
  before arming, `medeleg`=0 on readback.

S-mode phase (hart dropped to S-mode via `sret`, PMP NAPOT entry
opening the whole address space, `mcounteren`=0x7):

- `mtimecmp` armed to `mtime` + 500 ticks; the armed value reads
  back immediately (read before any slow UART output, since the
  50 us arm distance is shorter than printing one line).
- Exactly one M-mode trap: `mcause`=0x8000000000000007 (machine
  timer interrupt, code 7), `mtval`=0x0. The handler disarms
  `mtimecmp` to all-ones inside the trap, so the readback after is
  0xffffffffffffffff.
- Zero S-mode traps: the counting `stvec` entry never fires.
- A 2,000,000-`rdcycle` quiet window leaves the counts at
  m_traps=1, s_traps=0: one armed timer, one trap, no re-delivery.

## Measured results (3 runs, QEMU 8.2.2 `virt`, single hart)

Run logs: bench-logs/run1.log, bench-logs/run2.log, bench-logs/run3.log.
Build log: bench-logs/build.log.

All 3 runs exit 0 and are byte-identical except the
timing-dependent `arm:` line (verified by diff):

- boot: mideleg=0x1444; trap: mtvec=0x800001e8, stvec=0x80000244
  (this binary's layout, direct mode).
- mideleg: write=0x0 readback=0x1444.
- mideleg: write=0x80 readback=0x1444.
- mideleg: write=0x14c4 readback=0x1444.
- drop: mie=0x80 mideleg=0x1444.
- arm: mtimecmp=0x2b231 (run 1; varies run to run: it is
  `mtime`+500 at arm time).
- trap: m_traps=1 s_traps=0 mcause=0x8000000000000007 mtval=0x0.
- quiet: m_traps=1 s_traps=0.
- record checksum=0xc02ae00765d9bb85 (identical in all 3 runs;
  FNV-1a over boot readback, the three write readbacks, the two
  trap counts, and mcause, no timing-dependent fields).
- RESULT: PASS (checks=20).

## Verdict logic

PASS requires all 20 checks: `mideleg` reads 0x1444 at boot;
`mtvec`/`stvec` take the handler addresses in direct mode (2
checks); the three write/readback pairs each read back 0x1444 with
bit 7 clear; `medeleg` takes 0; `mtimecmp` disarms at setup;
`mip.MTIP` is clear before arming; `mie` reads back 0x80;
`mstatus.MIE` reads back set; `mideleg` bit 7 is still clear at
drop time; `mtimecmp` takes the armed value; the timer interrupt
arrives (bounded poll, no hang); exactly one M-mode trap with
`mcause` 0x8000000000000007; zero S-mode traps; `mtimecmp` is
all-ones after the in-handler disarm; the quiet window moves no
counts. A missing trap would park the hart (FAIL, timeout exit
124); an unexpected S-mode trap would fail the s_traps checks.

## Development notes (bugs found by measurement, fixed)

- The first build read `mtimecmp` back after printing the `arm:`
  line and failed its own arm check: the 500-tick (50 us) arm
  distance is shorter than printing one UART line at 115200 baud,
  so the timer had already fired and the handler had disarmed
  `mtimecmp` before the readback. The readback now happens
  immediately after the arm write, before any slow output.
- The first PASS build fell through to the FAIL print path on one
  run (a "RESULT: FAIL (checks=20 fails=0)" line after the PASS
  line): the finisher write does not always stop the hart before
  the next instruction executes. A `wfi` park loop now follows the
  finisher write, so no fall-through printing is possible.

## Limits, stated honestly

- This runs on QEMU 8.2.2's virt machine, an emulator, not on
  silicon. The WARL legalization (bit 7 read-only zero) and the
  routing behavior are this emulator's CSR model; real silicon
  publishes its own delegable set, and this module makes no claim
  about any other hart.
- The S-mode phase proves the interrupt is not delegated on this
  hart (0 S-mode arrivals while the hart sits in S-mode with the
  timer armed); it does not test every delegation bit, only bit 7.
- mtvec=0x800001e8 / stvec=0x80000244 are this exact binary's
  layout, published as a cross-check, not as portable facts.
