<!-- PROOF-HEADER
Checks: 5
Mismatches: 0
Checksum: 0xd2dc5b584cb8669
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mscratch csrrw atomic-swap round-trip (backlog item 173)

## What was built

`src/mscratch-csrrw/`: a bare-metal RISC-V program that exercises the
Zicsr `csrrw` atomic swap on `mscratch` in M-mode. Four files, about 300
lines total, sharing only `src/boot.S` and `src/uart.c` with the other
demos.

- `mscratch_main.c`: UART bring-up, the boot-value read, the swap and
  restore sequence, the five checks, the FNV-1a checksum over all
  verdict-relevant values, and the PASS/FAIL verdict.
- `msc_trap.S`: minimal M-mode trap entry that increments
  `msc_trapcount` and parks the hart. The module spec is zero traps, so
  any trap would end the run before the verdict lines print. The
  handler never touches mscratch, leaving the CSR under test
  undisturbed.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make mscratch-csrrw.elf` (added to `all` in the Makefile).
Run: `timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mscratch-csrrw.elf`
(or `make run-mscratch-csrrw`).

## Configuration under test

- Hart: single hart, M-mode (QEMU boots the ELF straight into M-mode
  with `-bios none` on the `virt` machine).
- `src/boot.S` was inspected and does not reference mscratch anywhere
  (it only sets the stack pointer, clears BSS, and calls main), so the
  value read in step 1 is QEMU's reset value for the CSR.

## The swap, step by step

Sentinel swapped in: `0xA5A55A5A5A5A5A5` (nonzero, 64-bit, not
byte-repeating).

| step | operation | observed |
|---|---|---|
| 1 | `csrr rd, mscratch` (boot value, first read) | `0x0` |
| 2a | `csrrw rd, mscratch, SENTINEL`: rd | `0x0` (the boot value; read side of the swap) |
| 2b | `csrrs x0` readback of mscratch | `0xA5A55A5A5A5A5A5` (the sentinel; write side of the swap) |
| 3a | `csrrw rd, mscratch, boot_value`: rd | `0xA5A55A5A5A5A5A5` (the sentinel; read side) |
| 3b | final `csrr` readback of mscratch | `0x0` (the boot value, restored exactly) |

The five checks are: swap rd == boot value, mscratch readback ==
sentinel, restore rd == sentinel, final readback == boot value, trap
counter == 0. 5 checks, 0 failures, in all three runs.

Trap count: 0 in every run. The trap handler would increment
`msc_trapcount` and halt the hart on any trap, so a taken trap could
not hide behind the verdict lines; all three logs print `traps: 0`
followed by `RESULT: PASS`.

## Stability across runs

All three runs printed byte-identical module output (md5 of the module
output, excluding QEMU's own `terminating on signal 15 from pid ...`
termination line, is `3171210cff5b91d7ba13bef7b9b41c9f` for all three;
that line's pid comes from the `timeout` wrapper's SIGTERM and is not
module output). The FNV-1a checksum printed by the program,
`0xd2dc5b584cb8669`, covers boot value, swap rd, swap readback,
restore rd, restore readback, and trap count, and is identical across
the three runs. The boot mscratch value was `0x0` in all three runs:
stable, and reported as measured rather than assumed.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's CSR model for `csrrw` on `mscratch`, not
  real silicon. The atomic-swap semantics (rd receives old CSR value,
  CSR receives rs) are architectural (unprivileged spec, Zicsr), but
  the observation that the swap is correctly implemented is only as
  good as QEMU's implementation.
- Only `mscratch` is swapped, only with one nonzero sentinel, only
  hart 0, only M-mode. WARL behavior, `csrrs`/`csrrc` set/clear
  semantics, and other CSRs are not tested; the module is deliberately
  that small.
- No throughput is measured; there is no genuine throughput number to
  report, so none appears in the header above.
- The boot value `0x0` is QEMU's reset state for mscratch under
  `-bios none`; a different firmware or platform may reset it
  differently.

## Reproduction

```
make mscratch-csrrw.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mscratch-csrrw.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; QEMU is terminated by `timeout` afterwards
because the bare-metal image never exits QEMU on its own).
