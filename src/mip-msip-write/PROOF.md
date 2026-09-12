<!-- PROOF-HEADER
Checks: 17
Mismatches: 0
Checksum: 0x283c26a67ca35455
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mip.MSIP software-write probe (backlog item "riscv mip-msip-write")

## Verified finding

On QEMU 8.2.2 `virt`, `mip` bit 3 (MSIP) is **not** software-writable
through the CSR. Writes via `csrsi`/`csrci` (immediate and register
forms) are ignored: the readback is unchanged and the bit never
sets. The bit is driven by the CLINT `msip` MMIO register instead,
which is the mechanism `src/mip-pending-no-trap/` exercises. The
backlog item's premise (writing 1 pends the interrupt) does not
hold on this QEMU, so the module ships the verified slice: the
attempted software write, a control proving the write path works,
and the pending-but-undelivered window with zero traps.

## What was built

`src/mip-msip-write/`: a bare-metal RISC-V program that attempts to
write `mip.MSIP` from M-mode software and records exactly what the
hardware does. Three files, sharing only `src/boot.S` and
`src/uart.c` with the other demos. Exactly one mechanism is under
test: the software write path of the `mip` CSR's MSIP bit.

- `mmsw_trap.S`: M-mode trap entry. Counts the trap into
  `mmsw_trap_count` and parks the hart without returning. No trap
  is expected (`mstatus.MIE` and `mie.MSIE` stay clear for the whole
  run), so any stray trap shows up as a harness timeout, and
  reaching the verdict line proves the counter stayed 0.
- `mmsw_main.c`: M-mode boot (`uart_init`, defensive `mtvec`,
  boot `mip`/`mie`/`mstatus` baselines), then the probe sequence:
  a control write of `mip.SSIP` (bit 1) through `csrsi`/`csrci`
  with readback verification, the attempted `csrsi` set of
  `mip.MSIP` (bit 3) with the unchanged readback published, a
  bounded 1,000,000-iteration spin window with the enables clear
  (pending MTIP undelivered, trap count 0), the attempted `csrci`
  clear of `mip.MSIP` with the unchanged readback published, and
  final enable/trap-count checks. A failed check prints `FAIL` and
  flips the verdict; `RESULT: PASS` is printed only when every
  check held. On PASS the machine is shut down via the virt
  test-device finisher so the QEMU process exits 0; on FAIL the
  hart parks.
- `README.md` (this module's index entry), plus `bench-logs/`
  with the build log and three raw QEMU run logs.

Build: `make mip-msip-write.elf` (added to `all` in the Makefile).
Run: `timeout 20 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mip-msip-write.elf`
(or `make run-mip-msip-write`).

## Distinction from the sibling module

`src/mip-pending-no-trap/` drives the same pending bit through the
CLINT `msip` MMIO register at `0x0200000` and shows it setting and
clearing in `mip`. That module tests the external interrupt
source; this module tests the CSR software write path. The two
modules test different mechanisms and share no behavior.

## Configuration under test

- Hart: mhartid = 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the module never leaves M-mode.
- `mstatus.MIE` stays clear and `mie` stays 0 for the whole run,
  so no interrupt can be taken regardless of pending state.
- Toolchain: Ubuntu gcc-riscv64-unknown-elf 13.2.0
  (`~/workspace/toolchains/ubuntu-rv64`), `-O2`.
- QEMU 8.2.2 `virt` machine, `LD_LIBRARY_PATH` pointed at the
  pinned QEMU libraries.

## Expected values: measured first, then asserted

The asserted constants were measured with a scratch probe
(`/tmp/msip-probe.c`, never committed) run once under the same
QEMU before the module was written. Verbatim probe transcript:

```
probe: mip WARL
boot mip=0x80
csrsi mip,2 -> 0x82
csrci mip,2 -> 0x80
csrsi mip,8 -> 0x80
csrci mip,8 -> 0x80
csrw mip,allones -> 0x26e6
csrw mip,0x80 -> 0x80
csrs mip,t0=8 -> 0x80
csrc mip,t0=8 -> 0x80
probe done
```

The probe shows: the immediate-form write path works (SSIP bit 1
sticks via `csrsi` and clears via `csrci`), while MSIP bit 3 is
ignored in every form (`csrsi`, `csrci`, `csrs`, `csrc`, and an
all-ones `csrw` which legalizes to 0x26e6 with bit 3 clear).
`mstatus` reads 0xa00000000 at boot (the read-only UXL/SXL fields
reporting 64-bit); `mie` reads 0x0.

## What was verified

All 17 checks, each a direct measurement:

1. Boot `mip` reads 0x80 (MTIP pends at boot; `mtimecmp` reads 0
   at reset).
2. Boot `mie` reads 0x0.
3. Boot `mstatus` reads 0xa00000000.
4. `mstatus.MIE` clear at boot.
5. `mie.MSIE` clear at boot.
6-7. `mtvec` takes the handler address in direct mode.
8. Control: `csrsi mip, 2` sets SSIP, readback 0x82.
9. Control: `csrci mip, 2` clears SSIP, readback 0x80.
10. `csrsi mip, 8` does not set MSIP (bit 3 reads 0).
11. The ignored MSIP write changes nothing else in `mip`
    (readback still 0x80).
12. After the 1,000,000-iteration spin window with the enables
    clear, `mip` still reads 0x80.
13. Trap count is 0 after the spin window.
14. `csrci mip, 8` changes nothing in `mip` (readback still 0x80).
15. `mie.MSIE` still clear at end.
16. `mstatus.MIE` still clear at end.
17. Trap count is 0 at end.

## Bench evidence

Three runs under QEMU 8.2.2, byte-identical logs
(`bench-logs/run1.log`, `run2.log`, `run3.log`), each:

```
mip-msip-write: mip.MSIP software-write probe
boot: mip=0x80 mie=0x0 mstatus=0xa00000000
mip: after csrsi set bit 1 readback=0x82 (expect 0x82)
mip: after csrci clear bit 1 readback=0x80 (expect 0x80)
mip: after csrsi set bit 3 readback=0x80 (expect 0x80, write ignored)
mip: after spin window readback=0x80 traps=0 (expect 0x80 / 0)
mip: after csrci clear bit 3 readback=0x80 (expect 0x80, write ignored)
VERDICT mip_boot=0x80 mie_boot=0x0 mip_ssip_set=0x82 mip_ssip_clear=0x80 mip_set=0x80 mip_hold=0x80 mip_clear=0x80 mie_final=0x0 traps=0 checksum=0x283c26a67ca35455
checks=17 fails=0
RESULT: PASS
```

The FNV-1a checksum covers the deterministic measured values
only (register readbacks and the trap counter), so it is
identical across runs: 0x283c26a67ca35455 in all three.
