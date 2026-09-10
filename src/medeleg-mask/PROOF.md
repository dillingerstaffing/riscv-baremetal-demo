<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: medeleg/mideleg writable-mask measurement (backlog item 84)

## What was built

`src/medeleg-mask/`: a bare-metal RISC-V program that measures the
writable-bit masks of the M-mode trap-delegation CSRs `medeleg` and
`mideleg` on the QEMU `virt` board, and verifies that trap behavior is
unchanged after writing and restoring them. Three files, sharing only
`src/boot.S` and `src/uart.c` with the other demos. Exactly one
mechanism is under test: which bits of the two delegation CSRs
survive an all-ones write, and whether trap delivery after a
write/restore cycle is byte-identical to before.

- `mdel_trap.S`: M-mode trap entry. Bumps a trap counter in
  `mdel_regs[0]`, records `mcause`/`mepc`/`mtval` in slots 2-4,
  advances `mepc` by 4 (skipping the trapping ecall), restores t0/t1,
  and returns with `mret`.
- `mdel_main.c`: installs direct-mode `mtvec` and `mscratch`, records
  the boot-time `medeleg`/`mideleg` values, takes a baseline M-mode
  ecall trap, writes all-ones to each CSR twice (the two readbacks
  must agree; the value itself is reported, never assumed), restores
  both CSRs to their recorded boot values and confirms the readback,
  then takes a second M-mode ecall trap and requires `mcause`,
  `mepc`, and `mtval` to match the baseline exactly. A failed check
  prints `FAIL` and flips the verdict; `RESULT: PASS` is printed only
  when every check held.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make medeleg-mask.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel medeleg-mask.elf`
(or `make run-medeleg-mask`).

Toolchain: xpack riscv-none-elf-gcc 15.2.0 (via
`~/workspace/toolchains/compat-bin`), QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- The program is the only code running; no S-mode, no interrupts
  enabled, no PLIC or CLINT involvement. Delegation has no live lower
  privilege mode to act on here: the module measures the CSR mask
  hardware and confirms trap delivery is unchanged, nothing more.

## Sequence and controls

1. Record boot-time `medeleg` and `mideleg`.
2. Baseline: one M-mode `ecall`. The handler records
   `mcause`/`mepc`/`mtval`; the observed values are saved.
3. Write `0xFFFFFFFFFFFFFFFF` to `medeleg`, read back; write again,
   read back; the two readbacks must be identical (in-run stability).
   Same for `mideleg`.
4. Restore both CSRs to their recorded boot values, read back to
   confirm.
5. Second M-mode `ecall` from the same instruction address;
   `mcause`, `mepc`, and `mtval` must equal the baseline exactly.

Note on step 4: the boot value of `mideleg` turned out to be nonzero
(`0x1444`), so restoring to zero would have changed the machine state
relative to boot. The program restores the recorded boot values,
which is the correct no-op; the output shows `medeleg` restored to
`0x0` and `mideleg` restored to `0x1444`.

## Measured results

Boot values and the 3-run stability table (identical on every run):

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| boot `medeleg` | `0x0` | `0x0` | `0x0` |
| boot `mideleg` | `0x1444` | `0x1444` | `0x1444` |
| `medeleg` readback after all-ones (write 1 / write 2) | `0xf0bfff` / `0xf0bfff` | identical | identical |
| `mideleg` readback after all-ones (write 1 / write 2) | `0x3666` / `0x3666` | identical | identical |
| restored `medeleg` / `mideleg` | `0x0` / `0x1444` | identical | identical |
| baseline trap mcause / mepc / mtval | `0xb` / `0x8000020a` / `0x0` | identical | identical |
| post-restore trap mcause / mepc / mtval | `0xb` / `0x8000020a` / `0x0` | identical | identical |
| verdict | PASS | PASS | PASS |

Read the mask bits as measured (no hard-coded expectations were in
the program):

- `medeleg` mask `0xf0bfff`: bits 0-9 (except reserved bit 10),
  11, 12, 13, 15, and 20-23. Bits 0-9, 11, 12, 13, 15 are the
  standard synchronous-exception delegation bits (the bit-10 gap and
  the missing bit 14 match the reserved bits in the privileged
  specification). Bits 20-23 are reserved by the specification but
  writable on this QEMU build; see limits below.
- `mideleg` mask `0x3666`: bits 1, 2, 5, 6, 9, 12, 13. Bits 1, 5, 9
  are the S-mode software/timer/external interrupt delegation bits;
  bits 2 and 6 are the VS-mode software/timer bits (this QEMU
  exposes the hypervisor extension).

The M-mode ecall trap reports `mcause = 0xb` (code 11, environment
call from M-mode), `mepc` = the address of the `ecall` instruction,
`mtval = 0x0`. These are the values actually observed, not asserted
in advance; the program's trap checks only require trap 2 to match
trap 1 exactly.

## Raw QEMU output

### Run 1 (bench-logs/mdel-run1.log)

```
medeleg-mask: medeleg/mideleg writable-bit measurement
boot: medeleg=0x0 mideleg=0x1444
trap1: traps=1 mcause=0xb mepc=0x8000020a mtval=0x0
mask: medeleg-write=0xffffffffffffffff readback1=0xf0bfff readback2=0xf0bfff
mask: mideleg-write=0xffffffffffffffff readback1=0x3666 readback2=0x3666
restore: medeleg=0x0 mideleg=0x1444
trap2: traps=2 mcause=0xb mepc=0x8000020a mtval=0x0
RESULT: PASS
done
```

### Run 2 (bench-logs/mdel-run2.log)

```
medeleg-mask: medeleg/mideleg writable-bit measurement
boot: medeleg=0x0 mideleg=0x1444
trap1: traps=1 mcause=0xb mepc=0x8000020a mtval=0x0
mask: medeleg-write=0xffffffffffffffff readback1=0xf0bfff readback2=0xf0bfff
mask: mideleg-write=0xffffffffffffffff readback1=0x3666 readback2=0x3666
restore: medeleg=0x0 mideleg=0x1444
trap2: traps=2 mcause=0xb mepc=0x8000020a mtval=0x0
RESULT: PASS
done
```

### Run 3 (bench-logs/mdel-run3.log)

```
medeleg-mask: medeleg/mideleg writable-bit measurement
boot: medeleg=0x0 mideleg=0x1444
trap1: traps=1 mcause=0xb mepc=0x8000020a mtval=0x0
mask: medeleg-write=0xffffffffffffffff readback1=0xf0bfff readback2=0xf0bfff
mask: mideleg-write=0xffffffffffffffff readback1=0x3666 readback2=0x3666
restore: medeleg=0x0 mideleg=0x1444
trap2: traps=2 mcause=0xb mepc=0x8000020a mtval=0x0
RESULT: PASS
done
```

## Limits

- Emulator, not silicon: the masks measured are QEMU 8.2.2's model of
  the `virt` board. On real hardware the writable bits of `medeleg`
  and `mideleg` are implementation-defined; a physical core may
  expose a different subset, may hard-wire reserved bits to zero, and
  may boot with different reset values.
- `medeleg` bits 20-23 read back as writable on this QEMU build even
  though the privileged specification reserves bits 16-63 of
  `medeleg`. This looks like an emulator modeling choice (no masking
  of those bits) rather than documented behavior; do not rely on it
  anywhere.
- The `mideleg` boot value `0x1444` (VS-mode delegation bits set) is
  QEMU's reset state, not a specification requirement.
- No live delegation was exercised: with no S-mode or VS-mode
  running, writing the masks cannot change any trap's destination.
  The module proves the mask hardware and that a write/restore cycle
  leaves trap delivery unchanged, which is the complete claim. A
  follow-up module could install an S-mode handler and confirm a
  delegated trap actually lands there.
