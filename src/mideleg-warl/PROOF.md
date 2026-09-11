<!-- PROOF-HEADER
Checks: 6
Mismatches: 0
Checksum: n/a (functional verification)
Throughput: n/a (functional verification)
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: mideleg WARL write/legalized-readback check

In M-mode on QEMU, `mideleg` is a WARL CSR: every write is accepted
and the readback is the legalized value, the subset of interrupt
causes the implementation can delegate. This module publishes the
write/legalized-readback pairs across 3 runs. The all-ones write
reads back 0x3666 and the zero write reads back 0x1444, so on this
hart the delegable interrupt causes are bits 1, 2, 5, 6, 9, 10, 12,
13, and bits 2, 6, 10, 12 are read-only-one (a zero write cannot
clear them). No interrupt source is armed and `mstatus.MIE` stays
clear, so no trap should ever fire; a defensive park-on-entry
handler makes any stray trap observable as a harness timeout.

## Note on the item's premise

The backlog item expected the boot value to be 0 and the zero
write to read back 0. The measurement on QEMU 8.2.2's virt machine
disproves both expectations: the boot readback is 0x1444 and the
zero write reads back 0x1444. This module ships the corrected,
measured finding, not the expected one. The verdict logic asserts
what was measured.

## What the hardware guarantees, and what is measured

- `mideleg` is WARL: the legalized readback is implementation
  defined. Which bits exist is a per-implementation fact published
  by the readback itself; the three published pairs are that fact
  for this hart.
- Why these values: on this hart the H extension is present, and
  QEMU 8.2.2's write path `rmw_mideleg64` (target/riscv/csr.c)
  masks every write to delegable_ints =
  S_MODE_INTERRUPTS | VS_MODE_INTERRUPTS | MIP_LCOFIP (0x2666),
  then forces on HS_MODE_INTERRUPTS = MIP_SGEIP | MIP_VSSIP |
  MIP_VSTIP | MIP_VSEIP (0x1444) (bit definitions in
  target/riscv/cpu_bits.h). An all-ones write therefore reads back
  0x2666 | 0x1444 = 0x3666; a zero write reads back the forced
  bits 0x1444. The measured readbacks match this path exactly.
- The mtvec handler install is verified (address takes, direct
  mode) so the defensive handler is really in place.
- The second all-ones write repeats 0x3666, proving the
  legalization is stable across writes, not a one-shot artifact.

## Measured results (3 runs, QEMU 8.2.2 `virt`, M-mode, single hart)

Run logs: bench-logs/run1.log, bench-logs/run2.log, bench-logs/run3.log.
Build log: bench-logs/build.log.

All 3 runs are byte-identical (verified by diff), QEMU exit 0:

- boot: mideleg=0x1444; trap: mtvec=0x800001c8 (direct mode,
  handler address).
- write-ones: write=0xffffffffffffffff, readback=0x3666.
- write-zero: write=0x0, readback=0x1444.
- write-ones-again: write=0xffffffffffffffff, readback=0x3666.
- RESULT: PASS (checks=6).

## Verdict logic

PASS requires all 6 checks: `mideleg` reads 0x1444 at boot; `mtvec`
takes the handler address in direct mode (2 checks); the all-ones
write reads back 0x3666; the zero write reads back 0x1444; the
second all-ones write reads back 0x3666 again. A stray trap would
park the hart (FAIL, timeout exit 124); none fired.

## Limits, stated honestly

- This runs on QEMU 8.2.2's virt machine, an emulator, not on
  silicon. The delegable set 0x3666 and the forced bits 0x1444 are
  this emulator's CSR model; real silicon publishes its own set,
  and this module makes no claim about any other hart.
- mtvec=0x800001c8 is this exact binary's layout, published as a
  measurement, not a claim about any other build.
- The module exercises CSR writes only. No interrupt is actually
  delegated or delivered; the readback bits are published as data,
  not demonstrated in a live-interrupt context.
