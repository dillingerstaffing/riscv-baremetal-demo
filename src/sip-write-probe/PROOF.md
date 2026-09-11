<!-- PROOF-HEADER
Checks: 20
Mismatches: 0
Checksum: 0xc2640b707a330c6f
Environment: QEMU 8.2.2
Verdict: PASS
-->
# Proof: sip WARL all-ones write/readback probe (backlog item 169)

## What was built

`src/sip-write-probe/`: a bare-metal RISC-V program that measures the
legalized value of the sip CSR after an all-ones write, in both
delegation states, on the QEMU `virt` board. Three files, sharing
only `src/boot.S` and `src/uart.c` with the other demos. Exactly one
mechanism is under test: which bits of sip survive a write of all
ones, and which bits are read-only aliases of mip.

- `swp_trap.S`: M-mode trap entry recording mcause/mepc/mtval and
  bumping a trap counter in the `swp_regs` array via mscratch. Both
  interrupt enables stay clear for the whole run, so the handler is
  expected never to fire; it advances mepc past the trapping
  instruction so a surprise trap cannot loop silently.
- `swp_main.c`: records the boot baselines (mie, mstatus, sip, mip,
  mideleg), installs direct-mode mtvec and mscratch, then runs
  phase 1 (SSI not delegated: `csrw sip, all-ones` must read back
  the boot baseline, `csrw sip, zero` must read back the baseline,
  mip must be unchanged across both writes), phase 2 (delegates SSI
  in mideleg, confirms the readback: `csrw sip, all-ones` must
  legalize to exactly SSIP set with every other bit unchanged,
  mip bit 1 must follow as the alias with all other mip bits
  unchanged, then `csrw sip, zero` must return sip byte-identical
  to the boot baseline and clear mip bit 1), and phase 3 (restores
  mideleg to the boot value, verifies mideleg and sip read back
  their baselines). A failed check prints `FAIL` and increments the
  mismatches counter; `RESULT: PASS` is printed only when every
  check held. The verdict-relevant values feed a 64-bit FNV-1a
  digest printed as the last data line, so the three bench runs can
  be compared for byte-identical output. On PASS the machine is shut
  down through the virt test-device finisher (QEMU exits 0); on FAIL
  the hart parks without touching the finisher.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make sip-write-probe.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel sip-write-probe.elf` (or `make run-sip-write-probe`), under
`timeout` so a parked-hart FAIL is observable as exit status 124.

Toolchain: Debian `riscv64-unknown-elf-gcc` 13.2.0,
`-march=rv64imac_zicsr`. QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: hart 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- mie.MSIE and mstatus.MIE read back clear at boot and at the end,
  so the asserted SSIP pending bit cannot be taken as a trap; the
  trap count must be 0.

## Measured results (3 QEMU runs, byte-identical)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs printed byte-identical output, `checks: 20 mismatches: 0`,
`digest: 0xc2640b707a330c6f`, `RESULT: PASS`, and QEMU exited 0
via the test-device finisher on every run.

| phase | operation | measured readback |
|---|---|---|
| boot | `csrr sip`, `csrr mip`, `csrr mideleg` | sip=0x0, mip=0x80, mideleg=0x1444 |
| 1: not delegated | `csrw sip, 0xffffffffffffffff` | sip=0x0 (write fully legalized away), mip=0x80 (unchanged) |
| 1: not delegated | `csrw sip, 0x0` | sip=0x0 (baseline), mip=0x80 (unchanged) |
| delegate | `csrw mideleg, 0x1446` | mideleg readback=0x1446 (SSI took) |
| 2: delegated | `csrw sip, 0xffffffffffffffff` | sip=0x2 (exactly SSIP set), mip=0x82 (bit 1 followed, 0x80 conserved) |
| 2: delegated | `csrw sip, 0x0` | sip=0x0 (byte-identical to boot baseline), mip=0x80 (bit 1 cleared) |
| restore | `csrw mideleg, 0x1444` | mideleg=0x1444, sip=0x0 |
| traps | handler installed, mie.MSIE=0, mstatus.MIE=0 throughout | trap count = 0 on every run |
| end | `csrr mie`, `csrr mstatus` | mie=0x0, mstatus=0xa00000000 |

Writable vs read-only, grounded in the readbacks: the only sip bit
that ever changed across an all-ones write was bit 1 (SSIP), and it
changed only while mideleg delegated SSI (phase 2 readback 0x2 vs
phase 1 readback 0x0 for the identical write). Bits 5 (STIP) and 9
(SEIP) never read nonzero and never changed, consistent with
read-only aliases of mip bits that were 0 at boot; mip's non-SSIP
bits (0x80, the M-mode MTIP) never appeared in any sip readback,
confirming sip exposes no M-mode interrupt bits. mip bit 1 tracked
sip bit 1 on every transition (0x80 -> 0x82 -> 0x80), which is the
direct observation of the alias in the delegated state.

## What was verified, and what was not

Verified: on QEMU 8.2.2, an all-ones write to sip legalizes to 0x0
while SSI is not delegated and to exactly 0x2 (SSIP only) once
mideleg delegates SSI; a zero write restores the exact boot
baseline; mip is never modified by sip writes, and mip bit 1 follows
sip bit 1; the three runs were byte-identical (digest
0xc2640b707a330c6f); no trap fired.

Not verified: behavior on real silicon. These numbers come from the
QEMU 8.2.2 CSR model, not from hardware. Also not verified:
delivery of an actual supervisor software interrupt (that would
require enabling SSIE/SIE, a separate mechanism, deliberately out
of scope), and the delegation behavior of the other supervisor
interrupt bits (STIP, SEIP).

## Limits

- The module assumes mideleg bit 1 is clear at boot (checked, and
  the run fails loudly if it is not) and that mideleg bit 1 is
  writable (checked via readback).
- The boot mideleg value 0x1444 is this emulator's choice; the
  module restores the recorded boot value rather than assuming
  zero.
