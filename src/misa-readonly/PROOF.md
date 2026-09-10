# Proof: misa WARL read-only check (backlog item 142)

## What was built

`src/misa-readonly/`: a bare-metal RISC-V program that checks whether
any bit of the `misa` CSR is writable on the QEMU `virt` board hart.
Two source files plus a build script, sharing only `src/boot.S` and
`src/uart.c` with the other demos. Exactly one mechanism is under
test: the writable-bit set of `misa`.

- `misa_trap.S`: M-mode trap entry. Saves t0 through mscratch,
  records `mcause`/`mepc`/`mtval`, bumps a trap counter, restores,
  and returns with `mret`. It is installed but expected never to
  fire; any trap fails the run.
- `misa_main.c`: installs direct-mode `mtvec` and `mscratch`, reads
  `misa` at boot as the baseline, writes all-ones
  (`0xFFFFFFFFFFFFFFFF`) with `csrw`, reads back, and requires the
  readback to be bit-identical to the boot value (the write was
  ignored; no writable bits). It then continues normal execution: a
  UART line and a deterministic sum of 1..100 (= 5050) prove the
  hart is unaffected. A failed check prints `FAIL` and flips the
  verdict; `RESULT: PASS` is printed only when every check held.
  On PASS it writes the virt test-device finisher word 0x5555 (QEMU
  exits 0); on FAIL it parks the hart in a `wfi` loop.
- `build.sh`: builds `misa-readonly.elf` at the repo root with the
  same flags as the repo Makefile (this module was not added to the
  root Makefile in this commit; it builds standalone).
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `sh src/misa-readonly/build.sh` (from the repo root).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel misa-readonly.elf`
under `timeout`.

Toolchain: riscv64-unknown-elf-gcc 13.2.0, QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- No interrupts enabled, no S-mode, no PLIC or CLINT involvement.
  The only CSR touched by the run besides the trap-handler setup is
  `misa` itself.

## Sequence and controls

1. Read `misa` at boot as the baseline. The value is reported, never
   assumed; the verdict is readback == boot, whatever the value is.
2. `csrw misa, 0xFFFFFFFFFFFFFFFF`.
3. Read `misa` back; require bit-identical equality with the boot
   value. Any difference means some bit is writable and fails the run.
4. Keep executing: print via UART and compute sum(1..100), requiring
   exactly 5050, showing the hart is unaffected.
5. Require the trap count to be 0 (the installed handler must never
   have fired; `csrw misa` is a legal M-mode write, so a trap would
   mean the hart was disturbed).

## Measured results

Identical on all three runs:

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| boot `misa` | `0x80000000001411ad` | identical | identical |
| written value | `0xffffffffffffffff` | identical | identical |
| `misa` readback after write | `0x80000000001411ad` | identical | identical |
| readback == boot value | yes | yes | yes |
| sum(1..100) after write | 5050 | identical | identical |
| trap count | 0 | 0 | 0 |
| verdict | PASS | PASS | PASS |

Reading the boot value as measured (no hard-coded expectation was in
the program): bit 63 set is MXL=2 (XLEN 64). The extension bitmap
`0x1411ad` has bits 0 (A), 2 (C), 3 (D), 5 (F), 7 (H), 8 (I),
12 (M), 18 (S), 20 (U) set. This is QEMU 8.2.2's `virt` default hart
configuration as observed, not an assertion.

## Raw QEMU output

### Run 1 (bench-logs/run1.log)

```
misa-readonly: misa WARL read-only check
boot: misa=0x80000000001411ad
write: value=0xffffffffffffffff readback=0x80000000001411ad
alive: sum1to100=5050
traps: count=0
RESULT: PASS
```

### Run 2 (bench-logs/run2.log)

```
misa-readonly: misa WARL read-only check
boot: misa=0x80000000001411ad
write: value=0xffffffffffffffff readback=0x80000000001411ad
alive: sum1to100=5050
traps: count=0
RESULT: PASS
```

### Run 3 (bench-logs/run3.log)

```
misa-readonly: misa WARL read-only check
boot: misa=0x80000000001411ad
write: value=0xffffffffffffffff readback=0x80000000001411ad
alive: sum1to100=5050
traps: count=0
RESULT: PASS
```

Build log (bench-logs/build.log):

```
built misa-readonly.elf
```

(the linker emitted its usual `LOAD segment with RWX permissions`
warning for this repo's link.ld; build exit 0.)

## Limits

- Emulator, not silicon: the read-only behavior measured is QEMU
  8.2.2's model of the `virt` board. On real hardware `misa` is
  WARL; a physical core could permit changing some bits at run time,
  and the boot value and writable set are implementation-defined.
- The run writes all-ones exactly once per boot. It does not probe
  individual bits (nothing about this measurement distinguishes
  "all bits read-only" from "some bits read-only and the rest also
  read-only" beyond the equality result: the readback equals the
  boot value, so no bit accepted the write).
- The write is attempted in M-mode only; the module says nothing
  about what lower privilege modes would observe.
