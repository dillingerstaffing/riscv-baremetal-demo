<!-- PROOF-HEADER
Checks: 19
Mismatches: 0
Checksum: 0x8844670da10ebc2d
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: satp MODE WARL probe (Bare / Sv39 / Sv48 / Sv57 / reserved)

## What was built

`src/satp-mode-warl/`: a bare-metal RISC-V program that probes the
`satp.MODE` field's WARL behavior on RV64, on the QEMU `virt` board.
Two files, sharing only `src/boot.S` and `src/uart.c` with the other
demos. Exactly one mechanism is under test: which MODE values the
hart's `satp` accepts, and what an unsupported MODE write does.

- `warl_trap.S`: M-mode direct-mode trap entry. No traps are
  expected on the happy path. If a trap fires it records
  `mcause`/`mepc`/`mtval` into C-visible globals, bumps the trap
  counter, prints the trap record and `RESULT: FAIL`, and parks the
  hart in a `wfi` loop.
- `warl_main.c`: M-mode boot (records the boot-time `satp` twice,
  installs the M-mode handler, opens the address space with one PMP
  NAPOT R/W/X entry because S-mode is default-deny, builds the page
  table, sanity-checks the layout) then runs the MODE discovery in
  M-mode, restores `satp`, and `mret`s with `mstatus.MPP=01` into the
  S-mode payload `warl_smode_test`, which proves Sv39 translation
  with a through-translation RAM read.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

The MODE discovery runs in M-mode deliberately: a MODE write that
sticks cannot fault anything there (M-mode accesses are never
translated, on this emulator or on silicon), so even Sv48/Sv57 are
safe to probe. The first version of this module probed MODE=9 in
S-mode with PPN=0; the write took effect and the next instruction
fetch faulted with `mcause=0xc` (instruction page fault,
`mepc=mtval=0x8000036e`, the faulting PC), which is converging
evidence that QEMU 8.2.2 accepts MODE=9 and attempts translation.
The discovery was moved to M-mode after that observation.

Build: `make satp-mode-warl.elf` (added to `all` and `clean` in the
Makefile). Run: `qemu-system-riscv64 -machine virt -nographic -bios
none -kernel satp-mode-warl.elf` (`make run-satp-mode-warl`).

## Measured results

Environment: QEMU emulator 8.2.2, `-machine virt`, `-bios none`,
`riscv64-unknown-elf-gcc` 13.2.0, `-O2`. Three runs, byte-identical
output, QEMU exit code 0 via the virt test-device finisher.

MODE discovery (M-mode, PPN=0, ASID=0), write/readback pairs:

| probe | prior | write | readback | outcome |
|---|---|---|---|---|
| MODE=0 (Bare) | 0x0 | 0x0 | 0x0 | readback == write |
| MODE=8 (Sv39) | 0x0 | 0x8000000000000000 | 0x8000000000000000 | MODE field stuck at 8 |
| MODE=9 (Sv48) | 0x8000000000000000 | 0x9000000000000000 | 0x9000000000000000 | STUCK (whole write took effect) |
| MODE=10 (Sv57) | 0x9000000000000000 | 0xa000000000000000 | 0xa000000000000000 | STUCK (whole write took effect) |
| MODE=15 (reserved) | 0xa000000000000000 | 0xf000000000000000 | 0xa000000000000000 | NO EFFECT (readback == prior) |

The MODE=15 row is the sharp one: the prior value was nonzero
(MODE=10 stuck), and the reserved-encoding write left the register
bit-identical, which pins the whole-write-ignored behavior rather
than mere MODE legalization.

Sv39 translation proof (S-mode): the two table entries verified
before enabling translation (`root_pt[2] = 0x200000cf`, PPN 0x80000
with V|R|W|X|A|D; `root_pt[0] = 0xc7`, PPN 0x0 with V|R|W|A|D);
`satp` written with MODE=8 and the table PPN
(`0x8000000000080002`) read back byte-identical with MODE field 8;
the RAM canary stored with translation off read through the active
translation at VA `0x80003110` as `0xc0ffee11deadbeef`, matching the
expected `0xc0ffee11deadbeef`; `satp` restored to Bare (readback
`0x0`).

Summary line: `checks: 19 mismatches: 0`, `checksum:
0x8844670da10ebc2d` (FNV-1a over every verdict-relevant value: boot
reads, each probe's prior/write/readback, the table PTEs, the Sv39
write/readback, the canary expected/got, the Bare readback, the trap
count, the final `satp`), `traps observed = 0`, final `satp = 0x0`
equal to the boot record, `RESULT: PASS`.

The 19 checks: boot record stable (1); table alignment and window
sanity (4); MODE=0 identity, MODE=8 field, MODE=9/10 clean
stick-or-no-effect, MODE=15 no-effect (5); `satp` restored after
discovery (1); S-mode entry `satp` == boot (1); table PTEs exact
(2); Sv39 MODE field 8 (1); canary match (1); Bare restored (1); 0
traps (1); final `satp` == boot record (1).

## Limits, stated honestly

- Everything above is QEMU 8.2.2 `virt` behavior, not silicon. A
  hart without Sv48/Sv57 would leave those writes without effect;
  the module's MODE=9/10 checks accept either clean outcome
  (readback == write, or readback == prior) so the probe documents
  rather than assumes.
- "MODE=9/10 stuck" means the MODE field read back the written
  value and the whole write took effect. The module does not verify
  that QEMU's Sv48/Sv57 walks are correct: no 4-level table was
  built, and the only translation attempted under MODE=9 (PPN=0, in
  the first design iteration) faulted as expected for a garbage
  root. The supported-and-proven scheme on this hart is Sv39.
- The through-translation read is identity-mapped (VA == PA). It
  proves the walk resolved to the intended frame (the canary stored
  before translation was enabled came back intact through the
  walk), not that VA != PA remapping works.
- The MODE=15 no-effect check ran with a nonzero prior, which is
  stronger than the zero-prior variant, but it still cannot
  distinguish "whole write ignored" from "every field independently
  legalized to its prior value"; both produce the observed
  readback. The claim is exactly the observed readback equality.
- Single hart; no interrupts enabled during the run. `sfence.vma`
  follows every `satp` write.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/satp-mode-warl/warl_main.c -o src/satp-mode-warl/warl_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o satp-mode-warl.elf src/boot.o src/uart.o src/satp-mode-warl/warl_trap.o src/satp-mode-warl/warl_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: satp-mode-warl.elf has a LOAD segment with RWX permissions
```

## Raw run logs (3 runs, byte-identical)

### run1.log

```
========================================
satp MODE WARL probe (Bare/Sv39/Sv48/Sv57/reserved)
========================================

satp at boot (M-mode reads) = 0x0 / 0x0

M-mode: satp.MODE discovery (PPN=0, ASID=0)

probe MODE=0 (Bare):
  prior    = 0x0
  write    = 0x0
  readback = 0x0
  outcome: readback == write

probe MODE=8 (Sv39):
  prior    = 0x0
  write    = 0x8000000000000000
  readback = 0x8000000000000000
  outcome: MODE field stuck at 8

probe MODE=9 (Sv48):
  prior    = 0x8000000000000000
  write    = 0x9000000000000000
  readback = 0x9000000000000000
  outcome: STUCK (whole write took effect)

probe MODE=10 (Sv57):
  prior    = 0x9000000000000000
  write    = 0xa000000000000000
  readback = 0xa000000000000000
  outcome: STUCK (whole write took effect)

probe MODE=15 (reserved):
  prior    = 0xa000000000000000
  write    = 0xf000000000000000
  readback = 0xa000000000000000
  outcome: NO EFFECT (readback == prior)

discovery done; satp restored to 0x0
setup complete; dropping to S-mode...
in S-mode; Sv39 translation proof begins

satp on S-mode entry = 0x0 (boot record = 0x0)
table: root_pt[2] = 0x200000cf (expect PPN 0x80000, flags V|R|W|X|A|D)
table: root_pt[0] = 0xc7 (expect PPN 0x0, flags V|R|W|A|D)
enable MODE=8 (Sv39), table PPN:
  write = 0x8000000000080002
  readback = 0x8000000000080002
  MODE field = 8
through-translation read: VA = 0x80003110 got = 0xc0ffee11deadbeef expected = 0xc0ffee11deadbeef
restore MODE=0 (Bare):
  readback = 0x0

traps observed = 0
final satp = 0x0 (boot record = 0x0)

checks: 19 mismatches: 0
checksum: 0x8844670da10ebc2d

COMPLETION MARKER: satp-mode-warl run finished
RESULT: PASS
```

### run2.log

```
========================================
satp MODE WARL probe (Bare/Sv39/Sv48/Sv57/reserved)
========================================

satp at boot (M-mode reads) = 0x0 / 0x0

M-mode: satp.MODE discovery (PPN=0, ASID=0)

probe MODE=0 (Bare):
  prior    = 0x0
  write    = 0x0
  readback = 0x0
  outcome: readback == write

probe MODE=8 (Sv39):
  prior    = 0x0
  write    = 0x8000000000000000
  readback = 0x8000000000000000
  outcome: MODE field stuck at 8

probe MODE=9 (Sv48):
  prior    = 0x8000000000000000
  write    = 0x9000000000000000
  readback = 0x9000000000000000
  outcome: STUCK (whole write took effect)

probe MODE=10 (Sv57):
  prior    = 0x9000000000000000
  write    = 0xa000000000000000
  readback = 0xa000000000000000
  outcome: STUCK (whole write took effect)

probe MODE=15 (reserved):
  prior    = 0xa000000000000000
  write    = 0xf000000000000000
  readback = 0xa000000000000000
  outcome: NO EFFECT (readback == prior)

discovery done; satp restored to 0x0
setup complete; dropping to S-mode...
in S-mode; Sv39 translation proof begins

satp on S-mode entry = 0x0 (boot record = 0x0)
table: root_pt[2] = 0x200000cf (expect PPN 0x80000, flags V|R|W|X|A|D)
table: root_pt[0] = 0xc7 (expect PPN 0x0, flags V|R|W|A|D)
enable MODE=8 (Sv39), table PPN:
  write = 0x8000000000080002
  readback = 0x8000000000080002
  MODE field = 8
through-translation read: VA = 0x80003110 got = 0xc0ffee11deadbeef expected = 0xc0ffee11deadbeef
restore MODE=0 (Bare):
  readback = 0x0

traps observed = 0
final satp = 0x0 (boot record = 0x0)

checks: 19 mismatches: 0
checksum: 0x8844670da10ebc2d

COMPLETION MARKER: satp-mode-warl run finished
RESULT: PASS
```

### run3.log

```
========================================
satp MODE WARL probe (Bare/Sv39/Sv48/Sv57/reserved)
========================================

satp at boot (M-mode reads) = 0x0 / 0x0

M-mode: satp.MODE discovery (PPN=0, ASID=0)

probe MODE=0 (Bare):
  prior    = 0x0
  write    = 0x0
  readback = 0x0
  outcome: readback == write

probe MODE=8 (Sv39):
  prior    = 0x0
  write    = 0x8000000000000000
  readback = 0x8000000000000000
  outcome: MODE field stuck at 8

probe MODE=9 (Sv48):
  prior    = 0x8000000000000000
  write    = 0x9000000000000000
  readback = 0x9000000000000000
  outcome: STUCK (whole write took effect)

probe MODE=10 (Sv57):
  prior    = 0x9000000000000000
  write    = 0xa000000000000000
  readback = 0xa000000000000000
  outcome: STUCK (whole write took effect)

probe MODE=15 (reserved):
  prior    = 0xa000000000000000
  write    = 0xf000000000000000
  readback = 0xa000000000000000
  outcome: NO EFFECT (readback == prior)

discovery done; satp restored to 0x0
setup complete; dropping to S-mode...
in S-mode; Sv39 translation proof begins

satp on S-mode entry = 0x0 (boot record = 0x0)
table: root_pt[2] = 0x200000cf (expect PPN 0x80000, flags V|R|W|X|A|D)
table: root_pt[0] = 0xc7 (expect PPN 0x0, flags V|R|W|A|D)
enable MODE=8 (Sv39), table PPN:
  write = 0x8000000000080002
  readback = 0x8000000000080002
  MODE field = 8
through-translation read: VA = 0x80003110 got = 0xc0ffee11deadbeef expected = 0xc0ffee11deadbeef
restore MODE=0 (Bare):
  readback = 0x0

traps observed = 0
final satp = 0x0 (boot record = 0x0)

checks: 19 mismatches: 0
checksum: 0x8844670da10ebc2d

COMPLETION MARKER: satp-mode-warl run finished
RESULT: PASS
```
