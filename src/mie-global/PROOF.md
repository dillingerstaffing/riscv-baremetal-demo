# Proof: mstatus.MIE bit gates machine software interrupt delivery (backlog item 113)

## What was built

`src/mie-global/`: a bare-metal RISC-V program that verifies the
MIE bit (bit 3) of the mstatus CSR gates delivery of the machine
software interrupt raised by the CLINT msip register on the QEMU
`virt` board. The per-source `mie.MSIE` bit stays set for the whole
run, so `mstatus.MIE` is the only gating variable. Three files,
sharing only `src/boot.S` and `src/uart.c` with the other demos.
Exactly one mechanism is under test: with MIE clear, an asserted
msip sits pending in mip and no trap fires; setting MIE via `csrw`
delivers exactly one machine software interrupt trap.

- `mig_main.c`: verifies boot state (mie == 0, mstatus.MIE == 0),
  installs a direct-mode mtvec handler with an mscratch scratch
  area, sets mie.MSIE (read back `0x8`), then runs phase 1 (MIE
  cleared via `csrc`, full mstatus readback, msip set, mip.MSIP
  verified pending before and after a bounded quiet window, zero
  traps allowed, msip cleared), phase 2 (msip set while MIE is
  still clear so the readback cannot race the handler, then MIE
  set via read-modify-`csrw` of mstatus; exactly one trap with
  mcause `0x8000000000000003`, handler-recorded mcause/mepc/mtval
  and trap-entry mstatus, no re-delivery), and a re-gate check
  (MIE cleared via `csrci`, fresh msip set traps nothing,
  mie.MSIE verified untouched). A failed check prints `FAIL` and
  flips the verdict; `RESULT: PASS` is printed only when every
  check held. On PASS the machine shuts down through the virt
  test-device finisher (QEMU exits 0); on FAIL the hart parks
  without touching the finisher.
- `mig_trap.S`: trap entry that saves t0/t1/ra/a0 through
  mscratch, records mcause/mepc/mtval, an entry cycle stamp, and
  the mstatus word as seen on trap entry, calls the C handler
  (clears msip first, since the source is level-triggered, then
  records the values and bumps the trap counter), restores, and
  returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make mie-global.elf` (added to `all` in the Makefile).
Run: `timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mie-global.elf`
with `~/workspace/qemu/usr/bin/qemu-system-riscv64` (8.2.2) and
`LD_LIBRARY_PATH=~/workspace/qemu/usr/lib/x86_64-linux-gnu:~/workspace/qemu/lib/x86_64-linux-gnu`
(the compat-bin QEMU is broken; missing libfdt/libfuse3).

## Configuration under test

- Hart: hart 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- CLINT msip for hart 0 at `0x02000000`, accessed as a 32-bit
  register. A 64-bit access was measured to fault on this QEMU
  (load access fault, cause 5, tval 0x02000000); see the defect
  note in `src/msip/PROOF.md`, backlog item 70. The established
  32-bit access form is used here.
- The machine software interrupt is the only enabled interrupt
  source: mie.MTIE and mie.MEIE are never set, so the mcause value
  and the trap counts are the ground truth for the gate.
- `mie.MSIE = 1` from setup to the end (read back `0x8` at both
  ends); mstatus.MIE is the only bit that changes mid-run.

## Sequence and controls

1. Control: at boot, mie must read `0x0` and mstatus.MIE must read
   0. The trap vector is installed and verified (direct mode),
   then mie.MSIE is set and read back (`0x8`).
2. Phase 1 (MIE clear): `csrc mstatus, 8`; the full mstatus
   readback must show bit 3 clear. msip is set to 1 (readback 1)
   and mip must show the MSIP bit (bit 3) set: the interrupt is
   pending. A bounded quiet window must leave the trap counter at
   0, proving the pending bit is gated off, and mip must still
   show MSIP set afterwards. msip is cleared (readback 0) and
   mip.MSIP must read 0.
3. Phase 2 (MIE set): msip is set to 1 first while MIE is still
   clear, so the set+readback cannot race the handler. Then MIE is
   set via read-modify-`csrw` of mstatus; the written and
   read-back full words are published and must match modulo the
   MPIE bit (see the MPIE note below), with bit 3 set. The trap
   must fire: the poll exits with spins < 10000000, mcause must be
   `0x8000000000000003`, the counter must be exactly 1, and the
   trap-entry mstatus must decode to MIE=0, MPIE=1, MPP=3. The
   handler must have cleared msip (reads 0) and mip.MSIP must read
   0; a further quiet window must leave the counter at 1 (no
   re-delivery).
4. Re-gate: `csrci mstatus, 8`; the full mstatus readback must show
   bit 3 clear. A fresh msip set (readback 1, mip.MSIP set) and
   quiet window must leave the counter at 1, proving the mstatus
   write alone stops delivery. msip is cleared, and mie must still
   read `0x8` (MSIE untouched).

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

The three run logs are byte-identical; the table shows run1, and
runs 2 and 3 matched every value.

| step | run1 | run2 | run3 |
|---|---|---|---|
| mie at boot (expect 0x0) | 0x0 | 0x0 | 0x0 |
| mstatus at boot (MIE expect 0) | 0xa00000000 | 0xa00000000 | 0xa00000000 |
| mtvec (handler installed, direct) | 0x800001c8 | 0x800001c8 | 0x800001c8 |
| mie after `csrs mie, 8` (expect 0x8) | 0x8 | 0x8 | 0x8 |
| phase1: mstatus after `csrc mstatus, 8` (MIE expect 0) | 0xa00000000 | 0xa00000000 | 0xa00000000 |
| phase1: msip readback after set (expect 1) | 1 | 1 | 1 |
| phase1: mip (MSIP expect 1, pending) | 0x88 | 0x88 | 0x88 |
| phase1: traps with MIE clear (expect 0) | 0 | 0 | 0 |
| phase1: mip after quiet (MSIP expect 1) | 0x88 | 0x88 | 0x88 |
| phase1: msip after clear / mip.MSIP (expect 0 / 0) | 0 / 0 | 0 / 0 | 0 / 0 |
| phase2: msip readback on set (expect 1) | 1 | 1 | 1 |
| phase2: mstatus written via csrw | 0xa00000008 | 0xa00000008 | 0xa00000008 |
| phase2: mstatus readback (MIE expect 1) | 0xa00000088 | 0xa00000088 | 0xa00000088 |
| phase2: poll spins to trap (expect < 10000000) | 0 | 0 | 0 |
| phase2: mcause (expect 0x8000000000000003) | 0x8000000000000003 | 0x8000000000000003 | 0x8000000000000003 |
| phase2: mepc | 0x80000564 | 0x80000564 | 0x80000564 |
| phase2: mtval | 0x0 | 0x0 | 0x0 |
| phase2: trap count (expect 1) | 1 | 1 | 1 |
| phase2: mstatus on trap entry (expect MIE=0 MPIE=1 MPP=3) | 0xa00001880 | 0xa00001880 | 0xa00001880 |
| phase2: msip after handler / mip.MSIP (expect 0 / 0) | 0 / 0 | 0 / 0 | 0 / 0 |
| phase2: traps after quiet (expect 1) | 1 | 1 | 1 |
| re-gate: mstatus after `csrci mstatus, 8` (MIE expect 0) | 0xa00000080 | 0xa00000080 | 0xa00000080 |
| re-gate: msip readback after set (expect 1) | 1 | 1 | 1 |
| re-gate: traps with MIE clear again (expect 1) | 1 | 1 | 1 |
| end: mie (expect 0x8, untouched) | 0x8 | 0x8 | 0x8 |
| QEMU exit code | 0 | 0 | 0 |
| RESULT | PASS | PASS | PASS |

What each value means:

- `mstatus` `0xa00000000` at boot: bit 3 (MIE) clear; the set high
  bits are `0xa << 32`, i.e. UXL (bits 33:32) and SXL (bits 35:34)
  both reading `10` = 64-bit, the read-only XLEN fields on this
  RV64 QEMU. They are constant through every readback in the run.
- Phase-1 trap count 0 with msip asserted, mie.MSIE = 1, a
  handler installed and verified, and mip.MSIP = 1 before and
  after the quiet window: the pending interrupt is gated off by
  the mstatus.MIE bit alone.
- `mip = 0x88`: bits 3 (MSIP) and 7 (MTIP). The MTIP bit reads 1
  because this QEMU's mtimecmp resets to 0 while mtime advances;
  it never matters because mie.MTIE stays clear for the whole run.
  All MSIP checks mask bit 3 only.
- Phase-2 mstatus written `0xa00000008`, read back
  `0xa00000088`: bit 7 (MPIE) appears in the readback even though
  the write did not set it. The pending interrupt traps between
  the `csrw` and the `csrr` instructions: hardware moves MIE into
  MPIE on trap entry, and `mret` sets MPIE=1 on return, so the
  readback carries MPIE=1. This is exactly the delivery the test
  is proving, and the check requires the write to have taken
  modulo MPIE. The first version of this module required exact
  equality and failed one check for this reason; the failure was
  the measurement, not a bug in the gate, and the check was
  corrected to mask MPIE.
- `spins-to-trap = 0`: the trap had already been taken before the
  poll loop's first iteration (delivery happens on the MIE-setting
  instruction itself), so the loop exits immediately; the check is
  `spins < 10000000`, and 0 satisfies it. This is prompt delivery,
  not a skipped poll. `mepc = 0x80000564` is the instruction after
  the `csrw mstatus`, consistent with the trap being taken as soon
  as the global bit went on with msip pending.
- Trap-entry mstatus `0xa00001880` decodes to MIE=0, MPIE=1,
  MPP=3: hardware auto-cleared the global bit on entry, saved the
  old MIE into MPIE, and recorded the M-mode previous privilege.
  Read from the CSR in the handler, not inferred.
- Traps after each quiet window unchanged (1 stays 1): the
  handler's msip clear ends the level-triggered source, and no
  spurious re-delivery occurs.
- Re-gate trap count still 1 after a fresh msip assertion with
  MIE clear: clearing the mstatus bit stops delivery again,
  symmetric with phase 1. `mie` still `0x8` at the end: MSIE was
  untouched, so MIE was the only gating variable.
- QEMU exit code 0 on all runs: the finisher shutdown path
  executed, i.e. `RESULT: PASS` with no parked FAIL.

## Toolchain note (measured, not assumed)

Built with the distro `riscv64-unknown-elf-gcc` 13.2.0 via the
repo Makefile flags (`-Wall -Wextra -O2 -march=rv64imac_zicsr
-mabi=lp64 -mcmodel=medany`). The build log in
`bench-logs/build.log` records the exact commands; the only
diagnostic is the linker's benign RWX-LOAD-segment warning also
seen on the sibling builds. No labels-as-values are used anywhere
in the module, so the documented 13.2.0 `&&label` miscompile does
not apply (all trap-resume behavior goes through `mret`, and no
trap-resume address is materialized in C).

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's CSR and CLINT models on the `virt`
  machine, not real silicon. The mstatus.MIE gating semantics are
  architectural; the 4-byte-only msip access width and the
  mtimecmp-reset-to-0 behavior are this model's implementation
  details (the msip width verified empirically in `src/msip/`).
- Only hart 0, only M-mode, only the machine software interrupt
  gate. Timer enable (MTIE), external enable (MEIE), S-mode
  delegation, and multi-hart delivery are not tested here; the
  module is deliberately that small.
- The quiet windows (2M spins) bound how long "no re-delivery" is
  observed; they are finite by construction (a bare-metal image
  never exits QEMU, so every wait needs a budget). A spurious
  re-delivery would have shown up as a counter change inside
  those windows.
- The three runs are byte-identical; no host-varying values
  appear in the check path.

## Reproduction

```
make mie-global.elf
export LD_LIBRARY_PATH="$HOME/workspace/qemu/usr/lib/x86_64-linux-gnu:$HOME/workspace/qemu/lib/x86_64-linux-gnu"
timeout 30 ~/workspace/qemu/usr/bin/qemu-system-riscv64 -machine virt -nographic -bios none -kernel mie-global.elf
```

Linked flat at 0x80000000 via `link.ld`. Build log:
`bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`.
Each ends with `RESULT: PASS (traps=1)` and the finisher shutdown
(exit 0).
