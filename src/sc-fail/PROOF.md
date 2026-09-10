<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: store-conditional failure-path experiment

## What was built

`src/sc-fail/`: a bare-metal RISC-V program, single hart, M-mode, that
checks the failure path of `sc.w`, complementing the shipped lr-sc
attempt-histogram module (which only ever observed the success path).
Two aligned 4-byte cells, `cell_a` and `cell_b`, are exercised by three
tests:

- (a) `sc.w` issued with NO preceding `lr.w` on `cell_a`.
- (b) `lr.w` on `cell_a` followed by `sc.w` on `cell_b` (a different
  address).
- (c) control: `lr.w` on `cell_a` immediately followed by `sc.w` on
  `cell_a`, the same shape as the histogram module's successful pairs.

A minimal M-mode trap handler (`scf_trap.S`) records
mcause/mepc/mtval into `scf_save` and halts the hart; no trap is
expected, and the program checks the handler's seen flag as part of
the verdict, so an unexpected trap would fail the run instead of
passing silently.

Three files, about 220 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos.

- `scf_main.c`: UART bring-up, trap vector installation, the three
  tests with before/after memory readbacks and the PASS/FAIL verdict.
- `scf_trap.S`: minimal M-mode trap entry. mscratch points at the
  4-word `scf_save` array; on entry it swaps t0, records
  mcause/mepc/mtval, flags the trap seen, restores t0, and parks the
  hart in a wfi loop.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make sc-fail.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -smp 1 -nographic -bios none -kernel sc-fail.elf`
(or `make run-sc-fail`).

## Spec ground truth being checked

The RISC-V privileged/instruction specification defines the
load-reserved/store-conditional mechanism around a reservation
register that is set only by an `lr` and only applies to the address
the `lr` touched. A store-conditional whose reservation is not live
for its target address (no preceding `lr`, or an `lr` on a different
address) must report failure in the destination register (nonzero)
without writing memory, and it is not a trap. The control test (c)
checks the complementary direction: `lr` followed by `sc` on the
same address reports success (rd == 0) and the stored value reads
back.

## Measured results (3 QEMU runs, all identical)

All three runs printed exactly the output below (mhartid = 0, single
hart):

```
sc-fail: store-conditional failure-path experiment
hart mhartid=0
trap vector installed (halts the hart on any trap)
test(a) sc.w with no lr.w:
  before=0xaaaaaaaa rd=1 after=0xaaaaaaaa
test(b) lr.w on A, sc.w on B (different address):
  before_a=0xaaaaaaaa before_b=0xbbbbbbbb
  rd=1 after_a=0xaaaaaaaa after_b=0xbbbbbbbb
test(c) control: lr.w then sc.w on same cell:
  rd=0 after_a=0x12345678
trap seen-flag=0
RESULT: PASS
done
```

- Test (a): rd = 1 (failure), target word unchanged
  (0xaaaaaaaa before and after).
- Test (b): rd = 1 (failure), both words unchanged
  (0xaaaaaaaa and 0xbbbbbbbb before and after).
- Test (c) control: rd = 0 (success), stored value 0x12345678
  reads back, proving the reservation mechanism works in this
  environment, so the nonzero rd in (a) and (b) is attributed to
  the missing/wrong reservation, not to `sc` being broken.
- Trap seen-flag = 0 in all 3 runs: no trap fired. The handler
  would have halted the hart and no RESULT line would have printed.

## Construction detail

Disassembly of the built ELF (`riscv64-unknown-elf-objdump -d
sc-fail.elf`) confirms the instruction shapes:

- Test (a): the `sc.w` at 0x800002cc stands alone; the nearest
  preceding instructions are plain `lw`/`lui`/`addiw` setup, no
  `lr.w` anywhere in the path.
- Test (b): `lr.w t1,(s0)` at 0x80000348 is immediately followed
  by `sc.w s6,a5,(s2)` at 0x8000034c, a different address register,
  with no instruction in between.
- Test (c): `lr.w t1,(s0)` at 0x800003f2 immediately followed by
  `sc.w s2,a5,(s0)` at 0x800003f6 on the same address.

The sc-result tests live outside the atomic windows, so they cannot
disturb the reservations. The C compiler is given no room to reorder
across the blocks (`"memory"` clobber, volatile).

## Emulator limit

These measurements were taken on QEMU 8.2.2 (`-machine virt`), not on
silicon. The reservation semantics observed (rd == 1 on a missing or
mismatched reservation, rd == 0 on a live one) are the emulator's
model of the spec rule stated above. No claim is made about any
particular hardware core.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sc-fail/scf_trap.S -o src/sc-fail/scf_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sc-fail/scf_main.c -o src/sc-fail/scf_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o sc-fail.elf src/boot.o src/uart.o src/sc-fail/scf_trap.o src/sc-fail/scf_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: sc-fail.elf has a LOAD segment with RWX permissions
```

Zero compiler warnings under `-Wall -Wextra` at `-O2`. The linker
RWX note is the same standard note every module in this repo
produces (flat memory map, link.ld).
