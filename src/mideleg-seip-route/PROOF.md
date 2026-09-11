<!-- PROOF-HEADER
Checks: 22
Mismatches: 0
Checksum: 0x44b4f8c94de3dbf9
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: mideleg SEIP-routing check

On QEMU 8.2.2's virt hart, `mideleg` is WARL with a legalized boot
readback of 0x1444, and bit 9 (0x200, the supervisor external
interrupt delegation bit) is in the delegable set: the write takes.
This module publishes the write/readback triple, pends the
supervisor external interrupt's own source (`mip.SEIP`, bit 9) from
M-mode, drops the hart to S-mode, and requires exactly one S-mode
trap with `scause = 0x8000000000000009` and zero M-mode traps.

## Note on the pend path

The supervisor external interrupt's pending bit is `mip` bit 9.
An S-mode `sip` write cannot pend it: the shipped `src/sip-seip-write`
module measured that S-mode writes to `sip` bit 9 are dropped
(read-only for S-mode). A pre-build probe measured that an M-mode
`csrs mip, 1<<9` sets the bit (readback 0x280 from 0x80, cleared
back to 0x80 by `csrc`), so this module pends SEIP in M-mode before
the drop, with a stable readback that must read exactly 0x200.

## Note on the handler's masking

The S-mode handler cannot clear SEIP either (same read-only
finding), so it masks instead: it clears `sstatus.SIE` and
`sstatus.SPIE` before `sret`. The SPIE clear is load-bearing: `sret`
restores SIE from SPIE, so clearing SIE alone would be undone on
return and the still-pending, level-triggered SEI would re-fire
immediately. The quiet window verifies the counts never move after
the one trap.

## What was measured

M-mode setup (all values read back, not assumed):

- `mtvec` takes the M-mode handler address in direct mode.
- boot: `mideleg`=0x1444.
- write 0x0 -> readback 0x1444 (the forced set).
- write 0x200 (bit 9 only) -> readback 0x1644: the write changed
  nothing but bit 9, and bit 9 was admitted.
- write 0x1444|0x200 -> readback 0x1644.
- `medeleg` takes 0x0; `stvec` takes the S-mode handler address in
  direct mode.
- `mie` reads back 0x0, `mstatus.MIE` reads back clear.
- `mtimecmp` disarmed (all-ones) on readback; `mip` reads 0x0
  afterwards (the boot-time MTIP pending bit is gone).
- `csrs mip, 1<<9`: `mip` reads back 0x200, SEIP alone pending.

S-mode phase (hart dropped to S-mode via `sret`, PMP NAPOT entry
opening the whole address space, `mcounteren`=0x7):

- `sie.SEIE` set on readback.
- Control: SEIP pending with SIE off; a 1,000,000-spin poll shows
  0 S-mode and 0 M-mode traps. The interrupt is masked.
- SIE set; the pending SEI is taken at the next instruction
  boundary. Exactly one S-mode trap: `scause`=0x8000000000000009
  (supervisor external interrupt, code 9), `sepc` equal to the
  interrupted instruction's address (captured with an in-assembly
  local label), `sip` showing SEIP (0x200) at handler entry.
- Zero M-mode traps.
- A 2,000,000-`rdcycle` quiet window leaves the counts at
  s_traps=1, m_traps=0: one pended SEI, one trap, no re-delivery.

## Measured results (3 runs, QEMU 8.2.2 `virt`, single hart)

Run logs: bench-logs/run1.log, bench-logs/run2.log, bench-logs/run3.log.
Build log: bench-logs/build.log.

All 3 runs exit 0 (finisher shutdown) and are byte-identical
(md5 `eb7fd6861ff0d18c585335588e377cc8` for all three logs):

- trap: mtvec=0x800001e4 (this binary's layout, direct mode).
- boot: mideleg=0x1444.
- mideleg: write=0x0 readback=0x1444.
- mideleg: write=0x200 readback=0x1644.
- mideleg: write=0x1644 readback=0x1644.
- medeleg: write=0x0 readback=0x0.
- trap: stvec=0x8000021c (this binary's layout, direct mode).
- pend: mip-after-csrs=0x200.
- s-mode: sie=0x200.
- control: s_traps=0 m_traps=0.
- sei: spins=0 s_traps=1 scause=0x8000000000000009
  sepc=0x800003bc expected=0x800003bc sip-at-entry=0x200.
- quiet: s_traps=1 m_traps=0.
- record checksum=0x44b4f8c94de3dbf9 (identical in all 3 runs;
  FNV-1a over the three write readbacks, the pend readback, the
  two trap counts, and scause; no timing-dependent fields).
- RESULT: PASS (checks=22).

## Verdict logic

PASS requires all 22 checks: `mtvec` takes the handler address;
`mideleg` reads 0x1444 at boot; the three write/readback pairs
read back 0x1444 / 0x1644 / 0x1644 with bit 9 admitted and nothing
else changed; `medeleg` takes 0; `stvec` takes the handler
address; `mie` reads back 0 and `mstatus.MIE` reads back clear;
`mtimecmp` disarms to all-ones; `mip` is clean before the pend;
the pend readback is exactly 0x200; `sie.SEIE` takes; the masked
control shows 0 traps of either kind; the SEI arrives (bounded
poll, no hang); exactly one S-mode trap with `scause`
0x8000000000000009; `sepc` equals the interrupted-instruction
address; `sip` at handler entry shows SEIP; 0 M-mode traps; the
quiet window moves no counts. A missing trap would park the hart
(FAIL, timeout exit 124); an unexpected M-mode trap would fail the
m_traps checks.

## Development notes (bugs found by measurement, fixed)

- The first build failed to assemble: `csrci sstatus, 0x20` is not
  encodable (CSR immediates are 5 bits; SPIE is bit 5). The
  handler now clears SIE and SPIE with one `csrrc` through a
  register (`li t1, 0x22; csrrc zero, sstatus, t1`).
- The first PASS build printed an unreachable `done` line after
  the parking loop; removed (dead code after `for (;;) wfi`).
- An unused-variable warning for a dropped `sipv` local was
  cleaned up; the build is warning-free.

## Limits, stated honestly

- This runs on QEMU 8.2.2's virt machine, an emulator, not on
  silicon. The WARL legalization (bit 9 admitted, forced bits
  0x1444) and the M-mode `mip` writability of SEIP are this
  emulator's CSR model; real silicon publishes its own delegable
  set and its own `mip` write mask, and this module makes no claim
  about any other hart.
- The S-mode phase proves the interrupt is delegated on this hart
  (1 S-mode arrival with `scause` 9, 0 M-mode arrivals while the
  hart sits in S-mode with SEIP pending); it does not test every
  delegation bit, only bit 9.
- mtvec=0x800001e4 / stvec=0x8000021c / sepc=0x800003bc are this
  exact binary's layout, published as a cross-check, not as
  portable facts.
- The `spins=0` value on the `sei:` line is not part of the
  checksum: it records that the trap had already been delivered
  and the done flag raised before the bounded poll started (the
  pending SEI is taken at the first instruction boundary after
  SIE is set).
