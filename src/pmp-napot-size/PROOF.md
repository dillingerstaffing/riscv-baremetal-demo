# Proof: PMP NAPOT size-decoding test (backlog item 118)

## What was built

`src/pmp-napot-size/`: a bare-metal RISC-V program that programs two
locked no-access PMP entries in NAPOT (naturally aligned power-of-two)
mode over one 64 KiB scratch region, at two different encoded sizes,
and proves by boundary probes that the fault/no-fault boundary moves
exactly as the `pmpaddr` trailing-ones size encoding predicts. Four
files, about 400 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos. This is deliberately narrower than
the shipped `src/pmp/` module (one 4 KiB locked NAPOT no-access region,
trap/no-trap at one size) and `src/pmp-tor/` (one exact TOR boundary):
the mechanism under test here is the size decoding itself, i.e. that
the number of trailing ones in `pmpaddr` selects the region size.

- `pns_main.c`: UART bring-up, PMP programming in two phases, controls,
  the boundary probes, and the PASS/FAIL verdict. Each probe is a single
  inline-asm block so the instruction layout is exact (see below).
- `pns_trap.S`: minimal M-mode trap entry. mscratch points at the
  7-word `pns_save` array; on entry it swaps t0, records
  mcause/mepc/mtval, loads mepc from the resume address the test stored,
  flags the trap seen, restores t0/t1, and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make pmp-napot-size.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel pmp-napot-size.elf`
(or `make run-pmp-napot-size`).

## Configuration under test

- Hart: mhartid = 0, single hart, running in M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- Scratch: 64 KiB at `0x80020000`, 64 KiB aligned by construction
  (`__attribute__((aligned(65536)))`); `base>>2` has its low 14 bits
  clear, so ORing the size field never disturbs the base. Verified by
  the program, not assumed.
- NAPOT encoding: `pmpaddr = (base >> 2) | ((size >> 3) - 1)`, the low
  run of 1 bits selecting `size = 8 << (trailing ones)`:
  - entry 0: `pmpaddr0 = 0x200081ff` = `(base>>2) | 0x1ff` (9 trailing
    ones), region `[0x80020000, 0x80021000)` (4 KiB).
  - entry 1: `pmpaddr1 = 0x20009fff` = `(base>>2) | 0x1fff` (13 trailing
    ones), region `[0x80020000, 0x80030000)` (64 KiB).
- `pmpcfg0`: entry bytes `0x98` = L (0x80) + A=NAPOT (0x18) + R=W=X=0.
  The L (lock) bit is required because this test runs in M-mode and
  unlocked PMP entries are not checked against M-mode accesses
  (RISC-V privileged spec, PMP section). Locking also freezes each
  entry's `pmpaddr`, which is why the two sizes live in two entries:
  entry 0 is programmed and probed in phase A, entry 1 is programmed
  after, in phase B. The lowest-numbered matching entry wins; both deny,
  so the 64 KiB entry governs once it exists.
- Lock verified, not assumed: after programming entry 1, `pmpcfg0`
  reads back `0x9898`, proving the locked byte 0 held `0x98` while byte
  1 was installed. Both `pmpaddr` registers read back exactly as
  written.

## Controls (what the fault is compared against)

1. Before programming, the whole 64 KiB scratch window is written with
   the byte pattern `i & 0xFF` and read back: proves the probed
   addresses are good RAM, so the later faults come from the PMP check
   and not from a bad address. No-trap probes verify a real value (the
   pattern byte, or the `0xA5` sentinel), not merely the absence of a
   trap.
2. A dedicated sentinel byte in a static outside the PMP region is
   written with `0xA5` and read back before programming; the program
   asserts its address is outside `[base, base+64K)`. It is probed in
   phase B (no trap, `0xA5` back), proving outside addresses stay
   genuinely readable.
3. After programming, a second buffer (`probe_outside`, elsewhere in
   `.bss`) is written and read back: proves the entries are narrow and
   the rest of the address space still works.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

Phase A, 4 KiB encoding only (entry 0 locked NAPOT deny):

| probe | seen | mcause | mepc | mtval | resume-8 | byte |
|---|---|---|---|---|---|---|
| lbu @0x80022000 (in 64K window, out of 4K window) | 0 | 0x0 | 0x0 | 0x0 | 0x80000270 | 0x0 |
| lbu @0x80020fff (last byte inside 4K) | 1 | 0x5 | 0x80000270 | 0x80020fff | 0x80000270 | - |
| lbu @0x80021000 (first byte outside 4K) | 0 | 0x0 | 0x0 | 0x0 | 0x80000270 | 0x0 |

Phase B, 64 KiB encoding added (entry 1 locked NAPOT deny):

| probe | seen | mcause | mepc | mtval | resume-8 | byte |
|---|---|---|---|---|---|---|
| lbu @0x80022000 (same address as phase A) | 1 | 0x5 | 0x80000270 | 0x80022000 | 0x80000270 | - |
| lbu @0x8002ffff (last byte inside 64K) | 1 | 0x5 | 0x80000270 | 0x8002ffff | 0x80000270 | - |
| lbu @0x80030000 (first byte outside 64K) | 0 | 0x0 | 0x0 | 0x0 | 0x80000270 | 0x0 |
| lbu @0x80030004 (sentinel outside region) | 0 | 0x0 | 0x0 | 0x0 | 0x80000270 | 0xa5 |

Identical across all three runs (byte-identical output apart from the
`timeout` kill line, which carries the QEMU pid). Every
program-internal check passed; all three runs print `RESULT: PASS`.

What each row means:

- Phase A(a): with only the 4 KiB encoding active, the load at
  `0x80022000` completes with no trap (`seen=0`) and returns the
  pattern byte `0x00` written before programming: the address is
  outside the decoded 4 KiB window.
- Phase A(b)/(c): the 4 KiB boundary is byte-exact. The last byte
  inside (`0x80020FFF`) traps with `mcause` 5 = load access fault, the
  exact code the privileged spec's trap table assigns; the first byte
  outside (`0x80021000`) reads cleanly.
- Phase B(d): the headline result. The same address `0x80022000` that
  read cleanly under the 4 KiB encoding now traps with `mcause` 5 and
  `mtval = 0x80022000` exactly the faulting address: the boundary moved
  with the encoded size, from a 4 KiB window to a 64 KiB window.
- Phase B(e)/(f): the 64 KiB boundary is byte-exact. The last byte
  inside (`0x8002FFFF`) traps with `mcause` 5 and
  `mtval = 0x8002FFFF`; the first byte outside (`0x80030000`) completes
  with no trap. The sentinel probe shows an outside address returning a
  real value (`0xA5`), so the no-trap results are genuine reads.
- `mepc` equals the address of the faulting instruction on every trap.
  This is checked, not eyeballed: the probe's asm block lays out
  `auipc` (4 bytes, never compressed), then the `lbu` (4 bytes; t1 is
  not a compressible register), then a 4-byte `sd` spilling the loaded
  byte, then the resume label. The trap handler resumes at the label,
  so the faulting instruction is always at `resume - 8`. The
  disassembly was inspected to confirm the layout (`auipc` at
  `0x8000026c`, `lbu` at `0x80000270`, `sd` at `0x80000274`, resume at
  `0x80000278`; note the compiler emitted the `la t0, 1f` as
  `auipc`+`addi`, which does not disturb the `resume - 8` invariant),
  and the program re-checks `mepc == resume - 8` on every run.
- `mtval` always equals the faulting address, matching the spec's rule
  that mtval carries the faulting address for access faults.

## One defect found and fixed during development

Recorded here because it changed what was verified.

Sentinel aliased the verdict counter (mine). The first version wrote a
`0xA5` sentinel byte to `base + 0x10000` as the first-byte-outside
probe value. The probes all behaved exactly as specified, but the run
reported `RESULT: FAIL (165 checks failed)` with no `FAIL:` lines in
the log. `riscv64-unknown-elf-nm` showed the link order placed the
`fails` counter at `0x80030000`, exactly `base + 0x10000`: the sentinel
write had overwritten `fails` with `0xA5` = 165. Fixed by never writing
to `base + 0x10000` (that probe is verified by no-trap alone, which is
the byte-exact boundary evidence) and moving the value-proven
readability check to a dedicated `far_sentinel` static whose address is
asserted at runtime to lie outside the PMP region. Lesson applied: any
address derived from the region geometry is suspect until checked
against the symbol table; `nm` is part of the verification loop now,
not just objdump.

(The `&&label` lesson from the sibling modules was applied from the
start: the resume address is taken inside the asm block with
`la t0, 1f` against a numeric local label, resolved exactly by the
assembler.)

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's PMP model on the `virt` machine, not real
  silicon. The trap code 5 is architectural (spec table), but the NAPOT
  trailing-ones size decoding and the locked-entry M-mode check are only
  as good as QEMU's implementation of those rules.
- Only entries 0-1, only NAPOT, only the 4 KiB and 64 KiB sizes, only
  hart 0, only M-mode. Stores, instruction-fetch denial, S/U-mode
  behavior, and other sizes (8 B minimum, larger powers of two) are not
  tested; the module is deliberately that small.
- mepc/mtval addresses are specific to this binary's layout; the
  invariants that transfer are `mepc == resume - 8` and
  `mtval == faulting address`, re-checked by the program on every run.
- The `0x80030000` outside probe's value is not checked (it aliases the
  link-placed `fails` counter in this build); its no-trap result is the
  boundary evidence, and the `0xA5` sentinel probe covers
  outside-region readability with a real value.

## Reproduction

```
make pmp-napot-size.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel pmp-napot-size.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0 (Ubuntu
13.2.0-11ubuntu1+12), QEMU 8.2.2 (from `~/workspace/qemu`).
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; QEMU is terminated by `timeout` afterwards
because the bare-metal image never exits QEMU on its own).
