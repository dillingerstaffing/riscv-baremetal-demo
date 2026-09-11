<!-- PROOF-HEADER
Checks: 14
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: sepc WARL software-write probe

Backlog item 123. In M-mode on QEMU, software writes of all-ones
and zero to `sepc` both take effect: the readbacks equal the
written values, so all 64 bits of the register are
software-writable on this hart. A deliberate M-mode `ecall`
(cause 11) then shows trap entry into M-mode leaves `sepc`
untouched: the register still reads the software-written 0, while
`mepc` (not `sepc`) carries the trap pc. No interrupt source is
armed and `mstatus.MIE` stays clear, so the deliberate ecall is
the only trap that can fire.

## Note on the item's premise

The backlog item's optional step suggested confirming that after
an M-mode ecall, `sepc` holds the ecall pc. The measurement
contradicts that expectation: the handler-recorded and
independently read-back `sepc` both equaled 0 after the trap,
while `mepc` equaled the ecall pc. This is the correct behavior
per the privileged spec: `sepc` is written only when a trap is
taken into S-mode; an M-mode trap writes `mepc`. This module
ships the corrected, measured finding, not the expected one. The
verdict logic asserts what was measured.

## What the hardware guarantees, and what is measured

- `sepc` is fully software-writable: a `csrw` is accepted and the
  readback equals the written value for every XLEN bit. Which bits
  stick is measured, not assumed.
- The trap mechanism writes `sepc` only on trap entry into
  S-mode. A trap taken into M-mode (here, the M-mode ecall, cause
  11) must leave the software-written value in place, and `mepc`
  must hold the trap pc.
- The write/readback pairs are the evidence for the
  writable-bits claim. The trap count must stay at 0 across both
  writes (a CSR write firing a trap would be visible), and reach 1
  only after the deliberate ecall.
- The ecall pc is captured with an in-asm numeric local label
  (`la t0, 1f` before `1: ecall`), not with C labels-as-values.

## Measured results (3 runs, QEMU 8.2.2 `virt`, M-mode, single hart)

Run logs: bench-logs/run1.log, bench-logs/run2.log, bench-logs/run3.log.
Build log: bench-logs/build.log.

All 3 runs are byte-identical (verified by diff):

```
sepc-warl: sepc software-write probe
boot: sepc=0x0
trap: mtvec=0x800001c8
write-ones: write=0xffffffffffffffff readback=0xffffffffffffffff
write-zero: write=0x0 readback=0x0
trap: ecall-pc=0x800003bc handler-mepc=0x800003bc handler-sepc=0x0 cause=0xb count=1
trap: sepc-readback=0x0 (expect 0x0)
trap: mepc-readback=0x800003c0 (expect ecall pc + 4)
RESULT: PASS (traps=1)
```

QEMU exit status: 0 on all 3 runs (finisher shutdown, no `FAIL`
line printed, all 14 checks passed).

## Verdict logic

PASS requires all of: `sepc` reads 0x0 at boot; `mtvec` takes the
handler address in direct mode; the all-ones write reads back
0xffffffffffffffff with trap count still 0; the zero write reads
back 0x0 with trap count still 0; the deliberate ecall traps
exactly once with handler cause 11, handler-recorded `mepc`
equal to the ecall pc (0x800003bc), handler-recorded and
independent-readback `sepc` still 0x0, and post-handler `mepc`
readback equal to ecall pc + 4 (0x800003c0).

## Limits, stated honestly

- This runs on QEMU 8.2.2's virt machine, an emulator, not on
  silicon. The all-bits-writable result is this emulator's CSR
  model; real silicon may fix some bits, and this module makes no
  claim about any other hart.
- ecall-pc=0x800003bc and mtvec=0x800001c8 are this exact binary's
  layout; they are published as measurements, not as claims about
  any other build.
- The module covers M-mode ecall traps only. `sepc` behavior on a
  trap taken into S-mode is not exercised here; the existing
  `sepc-resume-skip` module covers the S-mode resume path.
