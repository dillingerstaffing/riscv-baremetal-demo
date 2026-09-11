<!-- PROOF-HEADER
Checks: 14
Mismatches: 0
Checksum: 0x4b39a10748f739ad
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: mstatus.FS Off->Dirty transition on an FP register write (backlog item "riscv sstatus-fs-dirty")

Backlog item "riscv sstatus-fs-dirty": in M-mode on QEMU, write f1
and verify sstatus.FS transitions Off->Dirty, then clear via csrw
fcsr; publish CSR read triples across 3 runs. Two corrections to
that gloss were required by the hardware; see below.

## What was built

`src/sstatus-fs-dirty/`, a bare-metal M-mode binary sharing only
`src/boot.S` and the UART driver with the other demos. It walks
this sequence and checks every step in code:

1. Installs a counting M-mode trap handler (`sfd_trap.S`, direct
   mode, `mscratch` pointing at the trap record) as a safety net,
   clears `mstatus.MIE`, and asserts `mie == 0` at boot so no
   interrupt can fire.
2. Reads `misa` and requires the F and D extension bits, because
   `fmv.d.x` is a D-extension instruction.
3. Reads the boot `mstatus` and requires the FS field (bits 14:13)
   to read 0 (Off).
4. Sets FS to Initial (1) with `csrs mstatus`; requires the readback
   FS to be 1 with SD 0 and no other bit changed vs the baseline.
5. Executes one `fmv.d.x f1, x0`; requires FS to read back 3
   (Dirty), with SD 1 and no non-FS non-SD bit changed.
6. Executes a second `fmv.d.x f2, x0`; requires FS to stay 3
   (sticky Dirty).
7. Clears FS to 0 with `csrc mstatus`; requires the full `mstatus`
   word to read back identical to the boot baseline, then restores
   the boot word exactly with `csrw mstatus` and requires the
   readback to match.
8. Requires the trap counter to be 0.

The read triple (FS before the FP write, after it, cleared) is
printed, a 64-bit FNV-1a checksum over the eight logged
measurement words is printed, and `RESULT: PASS` prints only when
every check holds. On PASS the module writes the virt test-device
finisher word `0x5555` at `0x100000`, which shuts the machine down
(QEMU exits 0); on FAIL it parks the hart in a `wfi` loop without
touching the finisher, so under the harness's `timeout` a FAIL is
observable as exit status 124 as well as the `RESULT: FAIL` line.

The FP writes assemble under in-asm `.option arch, +d` because the
module builds with `-march=rv64imac_zicsr` (no F/D); the compiler
can never allocate f1/f2 under that march, so no register clobber
is needed. `src/boot.S` is first in the link order so `_start`
lands at 0x80000000.

Build integration: `Makefile` gains `sstatus-fs-dirty.elf` and
`run-sstatus-fs-dirty`; the module is in `all` and `clean`.

## Corrections to the backlog gloss

1. The gloss said "write f1 and verify sstatus.FS transitions
   Off->Dirty". With FS == Off the hardware does not record the
   write; executing an FP instruction with FS == Off raises an
   illegal-instruction exception instead. The honest sequence sets
   FS to Initial (1) first, and only then does the `fmv.d.x` move
   FS to Dirty (3). The counting trap handler (counter 0 on all 3
   runs) is the measurement that no exception fired anywhere in
   the sequence.
2. The gloss said "clear via csrw fcsr". `fcsr` carries no FS bits;
   FS lives in `mstatus` bits 14:13. The module clears FS with
   `csrc mstatus` and restores the baseline word with `csrw
   mstatus`.
3. The gloss named `sstatus.FS`. The module runs in M-mode, so it
   reads `mstatus` directly; `sstatus` is the S-mode view aliasing
   the same physical FS bits.

## Checks (all computed, none eyeballed)

1. `mtvec` took the handler address.
2. `mtvec` is in direct mode (base[1:0] clear).
3. `mie == 0` at boot.
4. `misa` carries the F (bit 5) and D (bit 3) extension bits.
5. Boot `mstatus` FS reads 0 (Off).
6. After `csrs`, FS reads 1 (Initial).
7. After `csrs`, no non-FS bit changed vs the baseline and SD
   reads 0.
8. After one `fmv.d.x` (f1), FS reads 3 (Dirty).
9. After that write, no non-FS non-SD bit changed vs the baseline
   and SD reads 1.
10. After a second `fmv.d.x` (f2), FS still reads 3 (sticky
    Dirty).
11. After the second write, no non-FS non-SD bit changed and SD
    still reads 1.
12. After `csrc`, the full `mstatus` word equals the boot baseline
    exactly.
13. After an explicit `csrw` of the baseline word, the readback
    equals the baseline exactly.
14. The trap counter is 0 (no trap fired).

## Measured read sequence

QEMU 8.2.2, `-machine virt`, 3 runs, byte-identical. `misa`
readback: `0x80000000001411ad` (F and D bits set). `mtvec`
readback: `0x800001c8` direct mode; `mie` readback: `0x0`.

| step            | `mstatus` readback   | FS | SD |
|-----------------|----------------------|----|----|
| boot baseline   | 0xa00000000          | 0  | 0  |
| after csrs      | 0xa00002000          | 1  | 0  |
| after fmv f1    | 0x8000000a00006000   | 3  | 1  |
| after fmv f2    | 0x8000000a00006000   | 3  | 1  |
| after csrc      | 0xa00000000          | 0  | 0  |
| after csrw base | 0xa00000000          | 0  | 0  |

Read triple (FS before the FP write, after it, cleared): 1, 3, 0.
Trap record: count=0, mcause=0x0, mepc=0x0, mtval=0x0.
`checks=14 mismatches=0`, `checksum=0x4b39a10748f739ad`,
`RESULT: PASS` on all 3 runs, QEMU exit code 0 on all 3 runs.

## Why SD read 1 on the Dirty reads

Bit 63 (SD) read 1 exactly on the two FS=3 readbacks and 0 on
every other readback. This is the same behavior measured by the
shipped `src/fs-check` module: QEMU 8.2.2's `write_mstatus`
recomputes SD as the OR-reduction of the dirty extension states,
so SD = 1 exactly when FS == 3. It matches the privileged
specification's definition of SD. No non-FS non-SD bit moved at
any step.

## Limits of verification

- Emulator, not silicon: every number above is a property of QEMU
  8.2.2's CSR and FP-translate implementation on this host, not of
  physical RISC-V hardware. A real core may implement the F
  extension differently (or not at all).
- The trap handler's mepc+4 skip is a safety net for this module's
  4-byte `fmv.d.x` only; a correct run never exercises it (counter
  stayed 0).
- Scope: this module measures the `mstatus` FS field's Off ->
  Initial -> Dirty -> Off sequence under real FP writes in
  M-mode. It does not test `sstatus`/VS-mode views, XS/VS dirty
  tracking, or FP data-path correctness.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sstatus-fs-dirty/sfd_trap.S -o src/sstatus-fs-dirty/sfd_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sstatus-fs-dirty/sfd_main.c -o src/sstatus-fs-dirty/sfd_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o sstatus-fs-dirty.elf src/boot.o src/uart.o src/sstatus-fs-dirty/sfd_trap.o src/sstatus-fs-dirty/sfd_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: sstatus-fs-dirty.elf has a LOAD segment with RWX permissions
```

The RWX warning comes from the shared `link.ld` (used by every
module in this repo) and is present in the other modules' build
logs too; no new warning was introduced by this module.

## Run logs (QEMU 8.2.2, all 3 runs)

Run 1 (run1.log):
```
sstatus-fs-dirty: FP register write moves mstatus.FS Off->Dirty
setup: mtvec=0x800001c8 mie=0x0
setup: misa=0x80000000001411ad
boot    : mstatus=0xa00000000 FS=0 SD=0
initial : mstatus=0xa00002000 FS=1 SD=0
after f1: mstatus=0x8000000a00006000 FS=3 SD=1
after f2: mstatus=0x8000000a00006000 FS=3 SD=1
cleared : mstatus=0xa00000000 FS=0 SD=0
traps: count=0 mcause=0x0 mepc=0x0 mtval=0x0
triple FS: before=1 after=3 cleared=0
checks=14 mismatches=0
checksum=0x4b39a10748f739ad
RESULT: PASS
```

Runs 2 and 3 (run2.log, run3.log) are byte-identical to run 1.
QEMU exit code 0 on all 3 runs (finisher shutdown path).
