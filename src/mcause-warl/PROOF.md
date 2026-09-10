<!-- PROOF-HEADER
Checks: 3 runs
Mismatches: 0
Checksum: n/a (QEMU run)
Throughput: n/a (functional verification)
Environment: QEMU 8.2.2
-->

# PROOF: mcause WARL software-write check

Backlog item 148. In M-mode on QEMU, a deliberate M-mode `ecall`
reports `mcause` = 11. Software writes of all-ones and zero to
`mcause` both take effect: the readbacks equal the written values,
so all 64 bits of the register are software-writable on this hart.
A second `ecall` then reports `mcause` = 11 again, proving trap
entry overwrites the register regardless of the last software
write. No interrupt source is armed and `mstatus.MIE` stays clear,
so the two deliberate ecalls are the only traps that can fire.

## Note on the item's premise

The backlog item expected the writes to be ignored (no
software-writable bits). The measurement on QEMU 8.2.2's virt
machine disproves that expectation: the all-ones write read back
`0xffffffffffffffff` and the zero write read back `0x0`. This
module ships the corrected, measured finding, not the expected
one. The verdict logic below asserts what was measured.

## What the hardware guarantees, and what is measured

- `mcause` is a WARL (write-any/read-legal) CSR: every write is
  accepted and the readback is a legal value. Which bits are
  writable is implementation-defined; this module measures it.
- The trap mechanism sets `mcause` on every trap entry. The two
  deliberate ecalls are environment calls from M-mode, so the
  cause is 11, confirmed both by the handler's record and by an
  independent `csrr` readback.
- The write/last-cause/readback triples are the evidence for the
  writable-bits claim: for each software write, the last trap
  cause is published alongside the write value and the readback.
- The trap count must stay at 1 across both writes (a CSR write
  firing a trap would be visible), and reach 2 only after the
  second deliberate ecall.

## Measured results (3 runs, QEMU 8.2.2 `virt`, M-mode, single hart)

Run logs: bench-logs/run1.log, bench-logs/run2.log, bench-logs/run3.log.
Build log: bench-logs/build.log.

All 3 runs are byte-identical (verified by diff):

- boot: mcause=0x0; trap: mtvec=0x800001c8 (direct mode, handler
  address).
- trap1: handler-cause=0xb, mepc=0x8000036c, count=1;
  mcause-readback=0xb.
- write-ones: write=0xffffffffffffffff, last-cause=0xb,
  readback=0xffffffffffffffff.
- write-zero: write=0x0, last-cause=0xb, readback=0x0.
- trap2: handler-cause=0xb, mcause-readback=0xb, count=2.
- RESULT: PASS (traps=2), QEMU exit 0 on all 3 runs.

## Verdict logic

PASS requires all of: `mcause` reads 0x0 at boot; `mtvec` takes the
handler address in direct mode; the first ecall traps exactly once
with handler cause 11 and independent readback 11; the all-ones
write reads back 0xffffffffffffffff with trap count still 1; the
zero write reads back 0x0 with trap count still 1; the second
ecall brings the count to 2 with handler cause 11 and readback 11.

## Limits, stated honestly

- This runs on QEMU 8.2.2's virt machine, an emulator, not on
  silicon. The all-bits-writable result is this emulator's CSR
  model; real silicon may fix some bits, and this module makes no
  claim about any other hart.
- mepc=0x8000036c and mtvec=0x800001c8 are this exact binary's
  layout; they are published as measurements, not as claims about
  any other build.
- The module covers M-mode ecall traps only. Interrupt causes
  (interrupt bit set, e.g. timer) are not exercised; the write
  test did set and read back bit 63, but only as a data bit, not
  in a live-interrupt context.
