<!-- PROOF-HEADER
Checks: 6
Mismatches: 0
Checksum: 0xdf03127f9aa56d
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: misa.MXL WARL legalization check (backlog item: riscv misa-mxl-warl)

## What was built

`src/misa-mxl-warl/`: a bare-metal RISC-V program that probes WARL
legalization on write of the `misa` CSR's MXL field. It differs from
`src/misa-readonly` (which only read the register) by attempting to
change the hart's XLEN encoding: it writes `misa` with MXL=1 (RV32)
while preserving every other bit, then checks that the field reads
back as 2 (RV64) and the extension bitmap is untouched.

- `mxl_trap.S`: M-mode trap entry. Saves t0 through mscratch,
  records `mcause`/`mepc`/`mtval`, bumps a trap counter, restores,
  and returns with `mret`. It is installed but expected never to
  fire; writing `misa` is a legal M-mode CSR write, even with an
  illegal MXL combination (WARL legalization), so any trap fails the
  run.
- `mxl_main.c`: installs direct-mode `mtvec` and `mscratch`, reads
  `misa` at boot as the baseline, writes
  `(boot & ~(3<<62)) | (1<<62)` (attempt MXL=1, preserve all other
  bits), reads back, and requires the MXL field `((v>>62)&3)` to
  still be 2 and bits 25:0 to be bit-identical to the boot value. It
  then writes the boot value back and requires the restore readback
  to equal the boot value exactly. A failed check prints `FAIL` and
  flips the verdict; `RESULT: PASS` is printed only when every
  check held. The program also prints an FNV-1a checksum over the
  verdict-relevant published values (boot, written, readback,
  restored, trap count), identical across all three runs
  (`0xdf03127f9aa56d`), so the runs are comparable byte-for-byte.
  On PASS it writes the virt test-device finisher word 0x5555 (QEMU
  exits 0); on FAIL it parks the hart in a `wfi` loop.
- `build.sh`: builds `misa-mxl-warl.elf` at the repo root with the
  same flags as the repo Makefile (this module was not added to the
  root Makefile in this commit; it builds standalone).
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `sh src/misa-mxl-warl/build.sh` (from the repo root).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel misa-mxl-warl.elf`
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
   assumed; the verdict is on the field and bitmap comparisons,
   whatever the value is.
2. `csrw misa, (boot & ~(3<<62)) | (1<<62)`: attempt the switch to
   32-bit while preserving the boot value's extension bitmap.
3. Read `misa` back. Require the MXL field to still read 2 (the hart
   legalized the illegal combination by ignoring the write to MXL)
   and bits 25:0 to be bit-identical to the boot value.
4. Write the boot value back; require the restore readback to equal
   the boot value exactly, confirming the register is back in its
   original state.
5. Require the trap count to be 0 (the installed handler must never
   have fired; `csrw misa` is a legal M-mode write even with an
   illegal MXL combination, so a trap would mean the hart was
   disturbed).
6. Compare the FNV-1a checksum across the three runs; they must be
   identical (`0xdf03127f9aa56d`).

## Measured results

Identical on all three runs:

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| boot `misa` | `0x80000000001411ad` | identical | identical |
| written value (MXL=1 attempt) | `0x40000000001411ad` | identical | identical |
| `misa` readback after write | `0x80000000001411ad` | identical | identical |
| MXL field after write | 2 (RV64) | identical | identical |
| extension bits 25:0 after write | `0x1411ad`, equal to boot | identical | identical |
| restore readback | `0x80000000001411ad` = boot | identical | identical |
| trap count | 0 | 0 | 0 |
| FNV-1a checksum | `0xdf03127f9aa56d` | identical | identical |
| verdict | PASS | PASS | PASS |

Reading the boot value as measured (no hard-coded expectation was in
the program): MXL=2 is XLEN 64. The write attempt changed only the
top two bits (MXL=1) and every one of them was ignored on readback;
the extension bitmap `0x1411ad` came back bit-identical. This is
QEMU 8.2.2's `virt` model legalizing a WARL write, observed, not
assumed.

## Raw QEMU output

### Run 1 (bench-logs/run1.log)

```
misa-mxl-warl: misa.MXL WARL legalization check
boot: misa=0x80000000001411ad
write: value=0x40000000001411ad
readback: misa=0x80000000001411ad
restore: misa=0x80000000001411ad
traps: count=0
checksum=0xdf03127f9aa56d
RESULT: PASS
```

### Run 2 (bench-logs/run2.log)

```
misa-mxl-warl: misa.MXL WARL legalization check
boot: misa=0x80000000001411ad
write: value=0x40000000001411ad
readback: misa=0x80000000001411ad
restore: misa=0x80000000001411ad
traps: count=0
checksum=0xdf03127f9aa56d
RESULT: PASS
```

### Run 3 (bench-logs/run3.log)

```
misa-mxl-warl: misa.MXL WARL legalization check
boot: misa=0x80000000001411ad
write: value=0x40000000001411ad
readback: misa=0x80000000001411ad
restore: misa=0x80000000001411ad
traps: count=0
checksum=0xdf03127f9aa56d
RESULT: PASS
```

Build log (bench-logs/build.log):

```
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: misa-mxl-warl.elf has a LOAD segment with RWX permissions
built misa-mxl-warl.elf
```

(the linker warning is this repo's link.ld as usual; build exit 0.)

## Limits

- Emulator, not silicon: the legalization behavior measured is QEMU
  8.2.2's model of the `virt` board. On real hardware `misa.MXL` is
  WARL; a physical core could in principle accept an MXL change at
  run time (in which case this exact program's execution path would
  change in observable ways), and the boot value and writable set
  are implementation-defined.
- The run attempts MXL=1 exactly once per boot. It does not probe
  other illegal combinations (e.g. MXL=3, reserved), and it does not
  distinguish "write ignored" from "write legalized to the same
  value" beyond the readback result.
- The write is attempted in M-mode only; the module says nothing
  about what lower privilege modes would observe.
