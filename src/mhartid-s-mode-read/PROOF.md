<!-- PROOF-HEADER
Checks: 11
Mismatches: 0
Checksum: 0xc3067c713d5fdc59
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: S-mode read of the M-mode-only mhartid CSR (backlog item "riscv mhartid-s-mode-read")

## What was built

`src/mhartid-s-mode-read/`: a bare-metal RISC-V program that checks
the privilege gate on the `mhartid` CSR on the QEMU `virt` board
hart. Three source files plus a build script, sharing only
`src/boot.S` and `src/uart.c` with the other demos. Exactly one
mechanism is under test: whether S-mode can read a CSR whose
number reserves it for M-mode.

The backlog premise guessed that an S-mode `csrr mhartid` would
succeed and agree with the M-mode value. That premise does not
survive contact with the architecture or the hart: `mhartid` is a
machine-mode CSR, and the measured run traps instead. This module
records the measured behavior.

- `mhs_mtrap.S`: M-mode trap entry. No M-mode trap is expected in
  a correct run, so the handler only bumps `mhs_mt_traps` and
  parks the hart; the bench harness observes the park as a
  timeout, so an unexpected M-mode trap fails the run.
- `mhs_strap.S`: S-mode trap entry. Swaps t0 through sscratch,
  records `scause`, `stval`, and the `sepc` value saved by the
  hardware at trap entry into the trap's slots, bumps the trap
  counter, then advances `sepc` by 4 and returns with `sret`.
  The faulting instruction is a `csrr`, which is always 32-bit,
  so the resume lands on the instruction immediately after it.
- `mhs_main.c`: M-mode setup reads `mhartid` twice as the
  baseline (expect agreement, and 0 on the virt boot hart),
  installs both trap vectors and `sscratch`, sets `medeleg`
  bit 2 so the illegal-instruction trap is delivered to S-mode,
  opens a whole-address-space PMP NAPOT entry, and drops to
  S-mode with `mret` (MPP=01). The S-mode payload presets a
  sentinel, executes `csrr mhartid` at a labeled site, and
  requires exactly one S-mode trap with `scause=0x2`, `sepc` at
  the fault site, the +4 resume at the labeled resume address,
  and the destination still holding the sentinel. A control
  `csrr sstatus` must then succeed with no new trap, and the
  M-mode trap count must still be zero. A failed check prints
  `FAIL` and flips the verdict; `RESULT: PASS` is printed only
  when every check held. On PASS it writes the virt test-device
  finisher word 0x5555 (QEMU exits 0); on FAIL it parks the hart
  in a `wfi` loop.
- `build.sh`: builds `mhartid-s-mode-read.elf` at the repo root
  with the same flags as the repo Makefile (this module was not
  added to the root Makefile in this commit; it builds
  standalone).
- `PROOF.md` (this file), `README.md`, `bench-logs/` with the
  build log and three raw QEMU run logs.

Build: `sh src/mhartid-s-mode-read/build.sh` (from the repo root).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel mhartid-s-mode-read.elf`
under `timeout`.

Toolchain: riscv64-unknown-elf-gcc 13.2.0, QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart (QEMU boots the ELF straight
  into M-mode with `-bios none`); the experiment itself runs in
  S-mode after an `mret` drop.
- `medeleg` bit 2 set, so the illegal-instruction trap is
  delivered to S-mode; all other traps stay in M-mode, where the
  handler counts and parks.
- One PMP NAPOT entry opens the whole address space to S-mode;
  no paging, no `satp`, no PLIC or CLINT involvement. The only
  CSRs whose access rules are under test are `mhartid` (phase A)
  and `sstatus` (the phase B control).

## Sequence and controls

1. In M-mode, read `mhartid` twice; require both reads to agree
   and to be 0, the value on the virt boot hart.
2. Install the M-mode and S-mode trap handlers and `sscratch`,
   write `medeleg` bit 2, and require the readback to show the
   delegation bit set, so the phase A trap destination is proven
   rather than assumed.
3. Open the whole-address-space PMP NAPOT entry and `mret` into
   S-mode.
4. Phase A: preset the destination register to
   `0xDEADBEEFDEADBEEF` and execute `csrr mhartid` at the labeled
   site. Require: exactly one S-mode trap, `scause=2` (illegal
   instruction), `sepc` equal to the fault site, the handler's
   advanced `sepc` equal to the labeled resume address, and the
   destination still equal to the sentinel.
5. Phase B (control): execute `csrr sstatus` in S-mode. Require
   no new trap and a destination overwritten by the read, so the
   phase A trap is charged to the `mhartid` privilege gate and
   not to CSR reads in general.
6. Require the M-mode trap count to still be zero: delegation
   delivered the one trap to S-mode and nothing fell through to
   M-mode.
7. An FNV-1a checksum over the baseline read, the check
   outcomes, the trap count, `scause`, `stval`, and the M-mode
   trap count fingerprints the run.

## Measured results

Identical on all three runs (md5 ef95ceddf9530c545227c399a8065bce
for each run log):

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| M-mode `mhartid` reads | `0x0` / `0x0` | identical | identical |
| `medeleg` readback | `0x4` | identical | identical |
| phase A trap count | 1 | identical | identical |
| phase A `scause` / `stval` | `0x2` / `0xf14024f3` | identical | identical |
| phase A `sepc` / `sepc+4` | `0x800002ec` / `0x800002f0` | identical | identical |
| phase A csrr destination | `0xdeadbeefdeadbeef` (sentinel) | identical | identical |
| phase B `sstatus` readback | `0x200000020` | identical | identical |
| traps after phase B | 1 | identical | identical |
| M-mode traps | 0 | identical | identical |
| FNV-1a checksum | `0xc3067c713d5fdc59` | identical | identical |
| checks / mismatches | 11 / 0 | identical | identical |
| verdict | PASS | PASS | PASS |

What was measured (not assumed): the S-mode read of `mhartid`
raises an illegal-instruction exception, exactly as the
privileged architecture prescribes for a CSR reserved for
M-mode. The faulting instruction word captured in `stval`
(`0xf14024f3`) decodes as `csrr mhartid` (funct12 `0xf14`); the
S-mode handler skipped it and the run continued. The read never
retired: the destination keeps the sentinel, and the M-mode
value `0x0` is never delivered to S-mode. The control read of
`sstatus` (`0x200000020`) completes in the same S-mode context
with no new trap, so the trap belongs to the `mhartid`
privilege gate. The sibling module `src/mhartid-readonly`
verified the write side of this register in M-mode; this module
covers the lower-privilege read that module explicitly left
unexercised.

## Raw QEMU output

### Run 1 (bench-logs/run1.log)

```
mhartid-s-mode-read: S-mode read of the M-mode hart ID register
boot: mhartid=0x0 second=0x0
medeleg readback=0x4
setup complete; dropping to S-mode...
in S-mode; mhartid read experiment begins

phase A: csrr mhartid in S-mode (expect S-mode trap):
  fault site (label 1)  = 0x800002ec
  resume site (label 2) = 0x800002f0
  scause  = 0x2
  stval   = 0xf14024f3
  sepc    = 0x800002ec
  sepc+4  = 0x800002f0
  traps   = 1
  csrr dest register = 0xdeadbeefdeadbeef (sentinel = 0xdeadbeefdeadbeef)

phase B: csrr sstatus in S-mode (expect success):
  sstatus readback = 0x200000020
  S-mode traps after phase B = 1

checksum (FNV-1a over verdict values) = 0xc3067c713d5fdc59
summary: checks=11 mismatches=0
RESULT: PASS
```

### Run 2 (bench-logs/run2.log)

Byte-identical to run 1 (verified with `cmp` and matching
md5sums, `ef95ceddf9530c545227c399a8065bce` for all three run
logs). The blank line at the head of the log is QEMU's UART
banner newline, present in every run.

### Run 3 (bench-logs/run3.log)

Byte-identical to run 1, same verification as run 2.

Build log (bench-logs/build.log):

```
built mhartid-s-mode-read.elf
exit=0
```

(the linker emitted its usual `LOAD segment with RWX permissions`
warning for this repo's link.ld; build exit 0.)

## Limits

- Emulator, not silicon: the trap behavior measured is QEMU
  8.2.2's model of the `virt` board. On real hardware `mhartid`
  is likewise M-mode-only per the architecture, so the same
  read must trap there too, but the exact trap manifestation
  (`stval` contents) is implementation-defined.
- The run exercises one S-mode read of one M-mode CSR. It does
  not probe U-mode reads, writes from S-mode, or any other
  machine CSR (`mstatus`, `mtvec`, and the rest each have their
  own access rules), and it says nothing about how an S-mode
  kernel should handle the trap in production.
- The trap was delivered to S-mode only because the module set
  `medeleg` bit 2; with delegation clear, the same fault would
  land in M-mode. That routing choice is setup, not part of the
  verified claim.
