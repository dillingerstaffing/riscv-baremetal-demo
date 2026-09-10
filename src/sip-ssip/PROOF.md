# Proof: sip SSIP (bit 1) is writable only while mideleg delegates the supervisor software interrupt

## What was built

`src/sip-ssip/`: a bare-metal RISC-V program that verifies the
writability rule of the sip SSIP pending bit (bit 1) on the QEMU
`virt` board. Three files, sharing only `src/boot.S` and
`src/uart.c` with the other demos. Exactly one mechanism is under
test: sip is a restricted view of mip, and bit 1 of sip is writable
through the sip CSR address only while mideleg bit 1 delegates the
supervisor software interrupt; when it is not delegated the bit
reads as zero and writes through sip are dropped.

- `sip_main.c`: records the boot baselines (mie, mstatus, sip, mip,
  mideleg), then runs phase A (SSIP not delegated: `csrs sip, 2`
  must be dropped, sip still 0x0), phase B (delegates bit 1 in
  mideleg, confirms the readback, then `csrs sip, 2` must set sip
  bit 1 with all other sip bits unchanged and mip bit 1 following
  with all other mip bits unchanged, then `csrc sip, 2` must return
  sip byte-identical to baseline and clear mip bit 1), and phase C
  (restores mideleg to the boot value, verifies sip reads the
  baseline again). A trap handler is installed but must never fire
  (mie.MSIE and mstatus.MIE read back clear at boot and at the end);
  any trap would be recorded (mcause/mepc/mtval) and fail the run.
  A failed check prints `FAIL` and flips the verdict; `RESULT:
  PASS` is printed only when every check held. On PASS the machine
  is shut down through the virt test-device finisher (QEMU exits
  0); on FAIL the hart parks without touching the finisher.
- `sip_trap.S`: M-mode trap entry recording mcause/mepc/mtval and a
  trap count in the `sip_regs` array via mscratch.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make sip-ssip.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel sip-ssip.elf` (or `make run-sip-ssip`), under `timeout`
so a parked-hart FAIL is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- QEMU 8.2.2 `virt` machine, Debian `riscv64-unknown-elf-gcc`
  13.2.0, `-march=rv64imac_zicsr`.
- Boot baselines measured by the module: `mie = 0x0`,
  `mstatus = 0xa00000000` (MIE clear), `sip = 0x0`, `mip = 0x80`
  (MTIP pending, conserved through the run), `mideleg = 0x1444`
  (bit 1, the supervisor software interrupt, not delegated).
- QEMU 8.2.2 implements the sip write mask as
  `(mideleg | mvien) & (SSIP | local-interrupt-bits)` (see
  `rmw_sip64` in `target/riscv/csr.c` of the v8.2.2 sources), so a
  write to sip bit 1 takes effect only when mideleg bit 1 is set.
  This is the ground truth the module verifies, not an assumption.

## Measured results (3 QEMU runs, byte-identical)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs printed the same values and `RESULT: PASS`, and QEMU exited 0
via the test-device finisher on every run.

| phase | operation | measured readback |
|---|---|---|
| boot | `csrr sip`, `csrr mip`, `csrr mideleg` | sip=0x0, mip=0x80, mideleg=0x1444 |
| A: not delegated | `csrs sip, 2` | sip=0x0 (write dropped), mip=0x80 (bit 1 clear) |
| B: delegate | `csrw mideleg, 0x1446` | mideleg readback=0x1446 (bit 1 took) |
| B: set | `csrs sip, 2` | sip=0x2 (SSIP=1, all other bits 0), mip=0x82 (bit 1 set, 0x80 MTIP conserved) |
| B: clear | `csrc sip, 2` | sip=0x0 (byte-identical to boot baseline), mip=0x80 (bit 1 clear) |
| C: restore | `csrw mideleg, 0x1444` | mideleg=0x1444, sip=0x0 |
| traps | handler installed, mie.MSIE=0, mstatus.MIE=0 throughout | trap count = 0 on every run |
| end | `csrr mie`, `csrr mstatus` | mie=0x0, mstatus=0xa00000000 |

The set/clear write used both the register form (`csrs sip, rs`)
and the result was confirmed through the mip cross-check: mip bit
1 tracks sip bit 1 exactly, which is the expected behavior of a
restricted view of mip.

## What was verified, and what was not

Verified: on QEMU 8.2.2, sip bit 1 is writable if and only if
mideleg delegates the supervisor software interrupt; the set makes
exactly bit 1 change in both sip and mip, and the clear restores
both CSRs to their exact boot values; no trap fired at any point
with the interrupt never enabled; mideleg restores cleanly.

Not verified: behavior on real silicon. These numbers come from
the QEMU 8.2.2 CSR model, not from hardware; an implementation is
free to make the delegation rule observable only through S-mode
accesses, and the boot mideleg value 0x1444 is this emulator's
choice. Do not treat emulator measurements as silicon behavior.
Also not verified: delivery of an actual supervisor software
interrupt (that would require enabling SSIE/SIE, a separate
mechanism, deliberately out of scope).

## Limits

- The module assumes mideleg bit 1 is clear at boot (checked, and
  the run fails loudly if it is not) and that mideleg bit 1 is
  writable (checked via readback).
- Only bit 1 of sip is exercised; the delegation rule for the
  other supervisor interrupt bits (STIP, SEIP) is a different
  mechanism and is not claimed here.
