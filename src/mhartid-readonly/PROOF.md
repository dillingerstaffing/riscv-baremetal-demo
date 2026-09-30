<!-- PROOF-HEADER
Checks: 7
Mismatches: 0
Checksum: 0x29c5e4e7cf447a7d
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mhartid read-only invariant (backlog item "riscv mhartid-readonly")

## What was built

`src/mhartid-readonly/`: a bare-metal RISC-V program that checks the
read-only invariant of the `mhartid` CSR on the QEMU `virt` board
hart. Three source files plus a build script, sharing only
`src/boot.S` and `src/uart.c` with the other demos. Exactly one
mechanism is under test: whether a `csrw` to `mhartid` can change
the value read back.

- `mhr_trap.S`: M-mode trap entry. Swaps t0 through mscratch, bumps
  a trap counter, records `mcause`, the `mepc` value saved by the
  hardware at trap entry, and `mtval` into that trap's 3-word slot
  (two slots are kept; further records are dropped rather than
  overflowing the array), then advances `mepc` past the faulting
  instruction (length-aware: 2 bytes for a 16-bit instruction, 4
  bytes otherwise) and returns with `mret`. The faulting
  instructions in this module are `csrw`, which is always 32-bit, so
  the resume lands on the instruction immediately after the `csrw`.
- `mhr_main.c`: installs direct-mode `mtvec` and `mscratch`, reads
  `mhartid` at boot as the baseline (expect 0 on the boot hart),
  attempts `csrw` all-ones (`0xFFFFFFFFFFFFFFFF`) to `mhartid`,
  reads back, attempts `csrw` zero, reads back, and requires both
  readbacks to be bit-identical to the boot value. The writes are
  expected to trap with an illegal-instruction exception
  (`mcause=0x2`) per the privileged spec; the run measures what
  actually happens instead of assuming it. A failed check prints
  `FAIL` and flips the verdict; `RESULT: PASS` is printed only when
  every check held. On PASS it writes the virt test-device finisher
  word 0x5555 (QEMU exits 0); on FAIL it parks the hart in a `wfi`
  loop.
- `build.sh`: builds `mhartid-readonly.elf` at the repo root with
  the same flags as the repo Makefile (this module was not added to
  the root Makefile in this commit; it builds standalone).
- `PROOF.md` (this file), `README.md`, `bench-logs/` with the build
  log and three raw QEMU run logs.

Build: `sh src/mhartid-readonly/build.sh` (from the repo root).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel mhartid-readonly.elf`
under `timeout`.

Toolchain: riscv64-unknown-elf-gcc 13.2.0, QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- No interrupts enabled, no S-mode, no PLIC or CLINT involvement.
  The only CSR touched by the run besides the trap-handler setup is
  `mhartid` itself.

## Sequence and controls

1. Install the trap handler and read `mhartid` at boot as the
   baseline. The value is reported, never assumed; the verdict is
   readback == boot, whatever the value is. A check also records
   that the boot value is 0, the expected value on the virt boot
   hart.
2. `csrw mhartid, 0xFFFFFFFFFFFFFFFF`.
3. Read `mhartid` back; require bit-identical equality with the boot
   value. Any difference means the write committed and fails the
   run.
4. `csrw mhartid, 0x0000000000000000`.
5. Read back again; require bit-identical equality with the boot
   value.
6. Examine the trap records: the two writes either both trapped
   (the spec's behavior for a write to a read-only CSR) or neither
   did (silently ignored). A trap count of anything other than 0 or
   2, or any recorded trap with `mcause != 0x2` (illegal
   instruction), fails the run.
7. An FNV-1a checksum over the readback triple, the trap count, and
   every recorded trap record fingerprints the run.

## Measured results

Identical on all three runs (md5 631861ac7ed47fbe62aca621b52cd1e7 for
each run log):

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| boot `mhartid` | `0x0` | identical | identical |
| readback after `csrw` all-ones | `0x0` | identical | identical |
| readback after `csrw` zero | `0x0` | identical | identical |
| readback == boot value (both) | yes | yes | yes |
| trap count | 2 | identical | identical |
| trap[0] `mcause` / `mepc` / `mtval` | `0x2` / `0x80000316` / `0xf1479073` | identical | identical |
| trap[1] `mcause` / `mepc` / `mtval` | `0x2` / `0x80000352` / `0xf1479073` | identical | identical |
| FNV-1a checksum | `0x29c5e4e7cf447a7d` | identical | identical |
| checks / mismatches | 7 / 0 | identical | identical |
| verdict | PASS | PASS | PASS |

What was measured (not assumed): each `csrw mhartid` raised an
illegal-instruction exception, exactly as the privileged spec
prescribes for a write to a read-only CSR. The faulting instruction
word captured in `mtval` (`0xf1479073`) decodes as `csrw mhartid`
(funct12 `0xf14`, funct3 `001`); the handler skipped it and the run
continued. The write never committed: both readbacks equal the boot
value `0x0`.

## Raw QEMU output

### Run 1 (bench-logs/run1.log)

```
mhartid-readonly: mhartid read-only invariant check
boot: mhartid=0x0
write: value=0xffffffffffffffff readback=0x0
write: value=0x0 readback=0x0
traps: count=2
trap[0]: mcause=0x2 mepc=0x80000316 mtval=0xf1479073
trap[1]: mcause=0x2 mepc=0x80000352 mtval=0xf1479073
checksum=0x29c5e4e7cf447a7d (FNV-1a over readbacks and trap records)
summary: checks=7 mismatches=0
RESULT: PASS
```

### Run 2 (bench-logs/run2.log)

```
mhartid-readonly: mhartid read-only invariant check
boot: mhartid=0x0
write: value=0xffffffffffffffff readback=0x0
write: value=0x0 readback=0x0
traps: count=2
trap[0]: mcause=0x2 mepc=0x80000316 mtval=0xf1479073
trap[1]: mcause=0x2 mepc=0x80000352 mtval=0xf1479073
checksum=0x29c5e4e7cf447a7d (FNV-1a over readbacks and trap records)
summary: checks=7 mismatches=0
RESULT: PASS
```

### Run 3 (bench-logs/run3.log)

Byte-identical to run 1 (verified with `cmp` and matching md5sums,
`631861ac7ed47fbe62aca621b52cd1e7` for all three run logs):

```
mhartid-readonly: mhartid read-only invariant check
boot: mhartid=0x0
write: value=0xffffffffffffffff readback=0x0
write: value=0x0 readback=0x0
traps: count=2
trap[0]: mcause=0x2 mepc=0x80000316 mtval=0xf1479073
trap[1]: mcause=0x2 mepc=0x80000352 mtval=0xf1479073
checksum=0x29c5e4e7cf447a7d (FNV-1a over readbacks and trap records)
summary: checks=7 mismatches=0
RESULT: PASS
```

Build log (bench-logs/build.log):

```
built mhartid-readonly.elf
```

(the linker emitted its usual `LOAD segment with RWX permissions`
warning for this repo's link.ld; build exit 0.)

## Limits

- Emulator, not silicon: the trap-on-write behavior measured is
  QEMU 8.2.2's model of the `virt` board. On real hardware
  `mhartid` is read-only per the spec, so the same write must not
  change the readback there either, but the exact trap
  manifestation (`mcause`, `mtval` contents) is
  implementation-defined.
- The run attempts two writes per boot (all-ones and zero). It does
  not probe intermediate values, and it says nothing about
  lower privilege modes (a U-mode or S-mode `csrw mhartid` would
  also trap, but on privilege grounds, and was not exercised).
- The write values were attempted only in M-mode; the module says
  nothing about delegation of the illegal-instruction trap.
