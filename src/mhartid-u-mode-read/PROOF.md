<!-- PROOF-HEADER
Checks: 14
Mismatches: 0
Checksum: 0x4504320e66bc7b23
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: U-mode read of the M-mode-only mhartid CSR (backlog item "riscv mhartid-u-mode-read")

## What was built

`src/mhartid-u-mode-read/`: a bare-metal RISC-V program that checks
the privilege gate on the `mhartid` CSR from U-mode, the lowest
privilege level, on the QEMU `virt` board hart. Three source files
plus a build script, sharing only `src/boot.S` and `src/uart.c`
with the other demos. Exactly one mechanism is under test: whether
U-mode can read a CSR whose number reserves it for M-mode. It is
the sibling of the shipped `src/mhartid-s-mode-read` module (the
S-mode case) and `src/mhartid-readonly` (the M-mode write side),
and follows their harness pattern exactly.

- `mhu_mtrap.S`: M-mode trap entry. The only M-mode trap the run
  plans is the U-mode `ecall` that ends the payload (mcause 8;
  only medeleg bit 2 is delegated, so the ecall lands in
  M-mode). On that path the handler restores `medeleg` and
  `pmpcfg0` to their boot values saved by `main`, writes the
  virt test-device finisher word 0x5555 when `mhu_result` is
  PASS, and parks the hart. Any other M-mode trap bumps
  `mhu_mt_traps` and parks, so the bench harness observes it as
  a timeout and the run fails loudly.
- `mhu_strap.S`: S-mode trap entry. Swaps t0 through sscratch,
  records `scause`, `stval`, and the `sepc` value saved by the
  hardware at trap entry into the trap's slots, bumps the trap
  counter, then advances `sepc` by 4 and returns with `sret`.
  Every faulting instruction the module executes in U-mode is a
  `csrr` of a privileged CSR, which is always 32-bit, so the
  resume lands on the instruction immediately after it.
- `mhu_main.c`: M-mode setup saves the boot values of `medeleg`
  and `pmpcfg0` (both read 0x0 on the virt board), reads
  `mhartid` twice as the baseline (expect agreement, and 0 on
  the virt boot hart), installs both trap vectors and
  `sscratch`, sets `medeleg` bit 2 so the illegal-instruction
  traps from U-mode are delivered to S-mode, opens a
  whole-address-space PMP NAPOT entry, and drops to U-mode with
  `mret` (MPP=00). The U-mode payload presets a sentinel,
  executes `csrr mhartid` at a labeled site, and requires
  exactly one S-mode trap with `scause=0x2`, `sepc` at the fault
  site, the +4 resume at the labeled resume address, and the
  destination still holding the sentinel. The control `csrr
  sstatus` (a CSR reserved for S-mode and above, also above
  U-mode) must then trap identically as a second S-mode trap
  with `scause=0x2`, so the phase A trap is charged to the
  privilege level and not to `mhartid` in particular. The
  M-mode trap count must still be zero. A failed check prints
  `FAIL` and flips the verdict; `RESULT: PASS` is printed only
  when every check held. The payload ends with an `ecall` into
  the M-mode exit handler: on PASS the finisher word shuts the
  machine down (QEMU exits 0); on FAIL the hart parks in a
  `wfi` loop (the harness sees exit 124).
- `build.sh`: builds `mhartid-u-mode-read.elf` at the repo root
  with the same flags as the repo Makefile (this module was not
  added to the root Makefile in this commit; it builds
  standalone).
- `PROOF.md` (this file), `README.md`, `bench-logs/` with the
  build log and three raw QEMU run logs.

Build: `sh src/mhartid-u-mode-read/build.sh` (from the repo root).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel mhartid-u-mode-read.elf`
under `timeout` (QEMU 8.2.2 at
`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

Toolchain: riscv64-unknown-elf-gcc 13.2.0, QEMU 8.2.2.

## Configuration under test

- Hart: mhartid = 0, single hart (QEMU boots the ELF straight
  into M-mode with `-bios none`); the experiment itself runs in
  U-mode after an `mret` drop with MPP=00.
- `medeleg` bit 2 set, so the illegal-instruction traps raised
  in U-mode are delivered to S-mode; the ending U-mode ecall
  (mcause 8) and every other trap stay in M-mode, where the
  handler finishes or counts and parks.
- One PMP NAPOT entry opens the whole address space to U-mode;
  no paging, no `satp`, no PLIC or CLINT involvement. The only
  CSRs whose access rules are under test are `mhartid`
  (phase A) and `sstatus` (the phase B control).

## Sequence and controls

1. In M-mode, save the boot values of `medeleg` and `pmpcfg0`
   (both read 0x0 on the virt board), then read `mhartid`
   twice; require both reads to agree and to be 0, the value on
   the virt boot hart.
2. Install the M-mode and S-mode trap handlers and `sscratch`,
   write `medeleg` bit 2, and require the readback to show the
   delegation bit set, so the trap destination is proven rather
   than assumed.
3. Open the whole-address-space PMP NAPOT entry and `mret` into
   U-mode.
4. Phase A: preset the destination register to
   `0xDEADBEEFDEADBEEF` and execute `csrr mhartid` at the labeled
   site. Require: exactly one S-mode trap, `scause=2` (illegal
   instruction), `sepc` equal to the fault site, the handler's
   advanced `sepc` equal to the labeled resume address, and the
   destination still equal to the sentinel.
5. Phase B (control): preset the destination register to the
   sentinel again and execute `csrr sstatus` in U-mode. `sstatus`
   is reserved for S-mode and above, so it must also raise an
   illegal-instruction trap in U-mode. Require a second S-mode
   trap with `scause=2`, `sepc` at its own fault site, the +4
   resume at its own resume label, and the destination still
   the sentinel: the phase A trap belongs to the privilege
   level, not to the CSR.
6. Require the M-mode trap count to still be zero: delegation
   delivered the two traps to S-mode and nothing fell through
   to M-mode.
7. The U-mode payload ends with an `ecall`. The M-mode handler
   restores `medeleg` and `pmpcfg0` to their boot values
   (0x0/0x0) and, on PASS, writes the virt test-device finisher
   word so QEMU exits 0. On FAIL it parks and the harness
   observes a timeout.
8. An FNV-1a checksum over the baseline reads, the boot CSR
   values, the check outcomes, the trap counts, `scause`,
   `stval`, and the M-mode trap count fingerprints the run.

## Measured results

Identical on all three runs (md5 3fbe3b0dee6335c0c9343bda353a7147
for each run log):

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| boot `medeleg` / `pmpcfg0` | `0x0` / `0x0` | identical | identical |
| M-mode `mhartid` reads | `0x0` / `0x0` | identical | identical |
| `medeleg` readback after set | `0x4` | identical | identical |
| phase A trap count | 1 | identical | identical |
| phase A `scause` / `stval` | `0x2` / `0xf1402973` | identical | identical |
| phase A `sepc` / `sepc+4` | `0x8000033c` / `0x80000340` | identical | identical |
| phase A csrr destination | `0xdeadbeefdeadbeef` (sentinel) | identical | identical |
| phase B trap count | 2 | identical | identical |
| phase B `scause` / `stval` | `0x2` / `0x10002973` | identical | identical |
| phase B `sepc` / `sepc+4` | `0x800004b0` / `0x800004b4` | identical | identical |
| phase B csrr destination | `0xdeadbeefdeadbeef` (sentinel) | identical | identical |
| M-mode traps | 0 | identical | identical |
| FNV-1a checksum | `0x4504320e66bc7b23` | identical | identical |
| checks / mismatches | 14 / 0 | identical | identical |
| verdict | PASS | PASS | PASS |

What was measured (not assumed): the U-mode read of `mhartid`
raises an illegal-instruction exception, exactly as the
privileged architecture prescribes for a CSR reserved for
M-mode, even at the lowest privilege level. The faulting
instruction words captured in `stval` decode as `csrr mhartid`
(0xf1402973, funct12 0xf14) and `csrr sstatus` (0x10002973,
funct12 0x100); the S-mode handler skipped each fault and the
run continued. Neither read retired: both destinations keep the
sentinel, and the M-mode value `0x0` is never delivered to
U-mode. The `sstatus` control traps identically, so the gate is
charged to the privilege level, not to the CSR itself. The
ending `ecall` restored `medeleg` and `pmpcfg0` to their boot
values (both 0x0, as read at boot) before the finisher word
shut the machine down. The sibling module
`src/mhartid-s-mode-read` verified the S-mode case of this same
gate; this module covers the U-mode case that module explicitly
left unexercised.

## Raw QEMU output

### Run 1 (bench-logs/run1.log)

```
mhartid-u-mode-read: U-mode read of the M-mode hart ID register
boot: medeleg=0x0 pmpcfg0=0x0
boot: mhartid=0x0 second=0x0
medeleg readback=0x4
setup complete; dropping to U-mode...
in U-mode; mhartid read experiment begins

phase A: csrr mhartid in U-mode (expect S-mode trap):
  fault site (label 1)  = 0x8000033c
  resume site (label 2) = 0x80000340
  scause  = 0x2
  stval   = 0xf1402973
  sepc    = 0x8000033c
  sepc+4  = 0x80000340
  traps   = 1
  csrr dest register = 0xdeadbeefdeadbeef (sentinel = 0xdeadbeefdeadbeef)

phase B: csrr sstatus in U-mode (expect S-mode trap):
  fault site (label 1)  = 0x800004b0
  resume site (label 2) = 0x800004b4
  scause  = 0x2
  stval   = 0x10002973
  sepc    = 0x800004b0
  sepc+4  = 0x800004b4
  traps   = 2
  csrr dest register = 0xdeadbeefdeadbeef (sentinel = 0xdeadbeefdeadbeef)

checksum (FNV-1a over verdict values) = 0x4504320e66bc7b23
summary: checks=14 mismatches=0
RESULT: PASS
```

### Run 2 (bench-logs/run2.log)

Byte-identical to run 1 (verified with `cmp` and matching
md5sums, `3fbe3b0dee6335c0c9343bda353a7147` for all three run
logs). The blank line at the head of the log is QEMU's UART
banner newline, present in every run.

### Run 3 (bench-logs/run3.log)

Byte-identical to run 1, same verification as run 2.

Build log (bench-logs/build.log):

```
built mhartid-u-mode-read.elf
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
- The run exercises one U-mode read of one M-mode CSR plus the
  `sstatus` control. It does not probe writes from U-mode, S/U
  reads of other machine CSRs, or how an S-mode kernel should
  handle the trap in production.
- The traps were delivered to S-mode only because the module
  set `medeleg` bit 2; with delegation clear, the same faults
  would land in M-mode. That routing choice is setup, not part
  of the verified claim. The boot values of `medeleg` and
  `pmpcfg0` were both 0x0 on this board and were restored
  before shutdown; on a board that boots with non-zero values
  the restore path would write those back instead.
