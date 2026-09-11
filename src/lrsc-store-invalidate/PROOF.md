<!-- PROOF-HEADER
Checks: 6
Mismatches: 0
Checksum: 0xa61fb63cece6cebc
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: intervening-store reservation-invalidation experiment

## What was built

`src/lrsc-store-invalidate/`: a bare-metal RISC-V program, single
hart, M-mode, on the QEMU `virt` board, checking that a plain store
to the reservation address between `lr.w` and `sc.w` invalidates the
reservation, so the `sc.w` reports failure where the
no-intervening-store case succeeds. Two aligned 4-byte cells
(`cell_a`, `cell_b`) are exercised by two tests:

- (a) `lr.w` on `cell_a`, then a plain `sw` of `0xBBBB1111` to the
  same address, then `sc.w` of `0xCCCC2222` on `cell_a`. The
  reservation must be gone: `sc` reports failure (`rd != 0`), the
  intervening store is visible (`cell_a == 0xBBBB1111`), and the
  failed `sc` stored nothing (`cell_a != 0xCCCC2222`).
- (b) control: `lr.w` on `cell_b` immediately followed by `sc.w` of
  `0xEEEE4444` on `cell_b`, the same shape as the shipped
  lrsc-histogram module's successful pairs. The `sc` must report
  success (`rd == 0`) and the stored value must read back. This
  control pins down that the reservation mechanism itself works in
  this environment, so the nonzero `rd` in (a) cannot be explained
  as "sc always fails here".

The three-instruction sequence in (a) is emitted as one `volatile`
asm block, so the compiler cannot reorder or delete the intervening
store; a disassembly check (below) confirms the emitted order is
`lr.w` / `sw` / `sc.w`. Four files, sharing only `src/boot.S` and
`src/uart.c` with the other demos: `lsi_main.c`, `lsi_trap.S`,
`PROOF.md` (this file), `bench-logs/` with the build log and three
raw QEMU run logs.

A minimal M-mode trap handler (`lsi_trap.S`) records
mcause/mepc/mtval into `lsi_save` and halts the hart; no trap is
expected (`lr.w`, `sw`, `sc.w` are all legal on aligned words), and
the program checks the handler's seen flag as part of the verdict,
so an unexpected trap would fail the run instead of passing
silently.

Build (toolchain flags match the Makefile; `riscv-none-elf-gcc` is
the xpack build of the same GNU toolchain, the `riscv64-unknown-elf-`
name is not installed on this machine): the exact commands are in
`bench-logs/build.log`. Run:
`qemu-system-riscv64 -machine virt -nographic -bios none -kernel lrsc-store-invalidate.elf`
under `timeout`.

## Configuration under test

- Hart: mhartid = 0, single hart, booted straight into M-mode with
  `-bios none`.
- QEMU 8.2.2
  (`~/workspace/qemu/usr/bin/qemu-system-riscv64`).
- Compiler: riscv-none-elf-gcc 15.2.0 (xpack), `-O2
  -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`, same flags as
  the Makefile.

## Measured results

Three runs under QEMU 8.2.2, byte-identical output, all
`RESULT: PASS`:

| Quantity                | Run 1        | Run 2        | Run 3        |
|-------------------------|--------------|--------------|--------------|
| `cell_a` before         | 0xAAAA0000   | 0xAAAA0000   | 0xAAAA0000   |
| test(a) `sc` rd         | 1 (failure)  | 1 (failure)  | 1 (failure)  |
| `cell_a` after          | 0xBBBB1111   | 0xBBBB1111   | 0xBBBB1111   |
| `cell_b` before         | 0xDDDD3333   | 0xDDDD3333   | 0xDDDD3333   |
| test(b) `sc` rd         | 0 (success)  | 0 (success)  | 0 (success)  |
| `cell_b` after          | 0xEEEE4444   | 0xEEEE4444   | 0xEEEE4444   |
| trap seen-flag          | 0            | 0            | 0            |
| checksum (FNV-1a)       | 0xa61fb63cece6cebc | 0xa61fb63cece6cebc | 0xa61fb63cece6cebc |
| checks / fails          | 6 / 0        | 6 / 0        | 6 / 0        |

Checks enforced in code (any failure prints `FAIL` with the count
and the verdict is not PASS): `rd_a != 0` (sc after an intervening
store reported failure); `after_a == 0xBBBB1111` (the intervening
store committed); `after_a != 0xCCCC2222` (the failed sc stored
nothing); `rd_b == 0` (control pair reported success);
`after_b == 0xEEEE4444` (control store visible); trap seen-flag
== 0 (no trap fired).

Disassembly check (via objdump of `lsi_main.o`): test(a) emitted
`lr.w t1,(a3)` / `sw a5,0(a3)` / `sc.w s2,a4,(a3)` in that exact
order, and test(b) emitted `lr.w t1,(s3)` / `sc.w s1,a5,(s3)`, so
the failure in (a) is tied to the intervening store and not to a
reordered or elided sequence.

Raw logs: `bench-logs/build.log`, `bench-logs/run1.log`,
`bench-logs/run2.log`, `bench-logs/run3.log`.

## Honest limits

- The invalidation premise reproduced on QEMU 8.2.2 (`rd == 1`
  after the intervening store, `rd == 0` without it). These are
  emulator measurements, not silicon: the RISC-V privileged
  specification permits, but does not require, a store to the
  reservation address to invalidate the reservation, so real
  hardware may behave differently. The experiment proves the
  observable outcome on this QEMU build, not the internal
  mechanism (address-based invalidation vs. value-compare failure)
  QEMU uses to produce it.
- Only one intervening-store shape is exercised (a plain `sw` to
  the same address by the same hart); stores to other addresses in
  the reservation set, stores by other harts, and interrupts
  between `lr` and `sc` are not covered here.
- Single hart, M-mode only; `sc.w` on 4-byte aligned cells only.
