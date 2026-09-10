# Proof: CLINT msip write drives the mip MSIP pending bit (backlog item 111)

## What was built

`src/mip-msip/`: a bare-metal RISC-V program that verifies the
CLINT msip register drives the MSIP pending bit (bit 3) of the mip
CSR on the QEMU `virt` board, with the interrupt itself never
enabled. One file, 214 lines, sharing only `src/boot.S` and
`src/uart.c` with the other demos. Exactly one mechanism is under
test: writing 1 to CLINT msip for hart 0 sets mip bit 3, and writing
0 clears it again, while `mie.MSIE` and `mstatus.MIE` stay clear for
the whole run (both read back at boot and at the end) so no trap can
fire. There is no trap handler in the image.

- `mmsp_main.c`: verifies boot state (interrupt disabled, mip.MSIP
  clear, msip reads 0), runs two msip set/clear cycles with a readback
  of msip and a full mip read on every transition, and requires that
  every non-MSIP bit of mip stays byte-identical across the run. A
  failed check prints `FAIL` and flips the verdict; `RESULT: PASS` is
  printed only when every check held. On PASS the machine is shut
  down through the virt test-device finisher (QEMU exits 0); on FAIL
  the hart parks without touching the finisher.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make mip-msip.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel mip-msip.elf`
(or `make run-mip-msip`).

## Configuration under test

- Hart: hart 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- CLINT msip for hart 0 at `0x02000000`, accessed as a 32-bit
  register (this QEMU's CLINT model only accepts 4-byte accesses to
  msip; see the defect note in `src/msip/PROOF.md`, backlog item 70).
- `mie.MSIE = 0` and `mstatus.MIE = 0` at boot, read back at boot
  and again after both cycles. No trap vector is installed, and with
  both enable bits clear no trap can be taken in between.
- QEMU 8.2.2 `virt` machine, xPack `riscv-none-elf-gcc` 15.2.0
  (see the toolchain note below).

## Sequence and controls

1. Control: at boot, `mie.MSIE` and `mstatus.MIE` must read 0, the
   msip register must read 0, and mip bit 3 (MSIP) must read 0. The
   module also reads the full mip word and records the non-MSIP bits
   (`0x80` on all three runs; see the finding below).
2. Cycle 1: write 1 to msip (32-bit), read it back (must be 1), read
   mip: bit 3 must be 1 and every other mip bit must be unchanged.
   Write 0 to msip, read back (must be 0), read mip: bit 3 must be 0
   again, other bits unchanged.
3. Cycle 2: the same set/clear again, proving the pending bit tracks
   the register repeatably.
4. Re-read `mie` and `mstatus` and require both enable bits still
   clear, proving the run's transitions were pure register-to-CSR
   effects, not interrupt delivery.

## One finding caught by measurement

The module was first written assuming `mip == 0` at boot; the first
run reported `mip = 0x80` with the MTIP check (bit 7) holding. On the
`virt` CLINT, `mtimecmp` for hart 0 resets to `0x0` while `mtime`
is already nonzero at first read (`0xff10`, `0x5c070`, `0x1f5e1`
across the three runs), so the machine timer pending bit is set from
boot. The program now verifies the attribution directly: it reads
`mtime` and `mtimecmp0` and requires `mip` bit 7 to equal the boolean
`(mtime >= mtimecmp)`, which held on all three runs. The module treats
bit 3 (MSIP) as the signal and all other mip bits as a conserved
mask, requiring them byte-identical on every transition; an
unrelated pending bit can therefore not leak into the verdict.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

| step | run1 | run2 | run3 |
|---|---|---|---|
| `mie.MSIE` / `mstatus.MIE` at boot | 0 / 0 | 0 / 0 | 0 / 0 |
| mip at boot | 0x80 | 0x80 | 0x80 |
| msip at boot (expect 0) | 0 | 0 | 0 |
| non-MSIP mip bits at boot | 0x80 | 0x80 | 0x80 |
| mtime at first read | 0xff10 | 0x5c070 | 0x1f5e1 |
| mtimecmp0 (expect 0) | 0x0 | 0x0 | 0x0 |
| mip bit 7 == (mtime >= mtimecmp) | yes | yes | yes |
| msip readback after set 1 (expect 1) | 1 | 1 | 1 |
| mip after set 1 (expect 0x88) | 0x88 | 0x88 | 0x88 |
| msip readback after clear 1 (expect 0) | 0 | 0 | 0 |
| mip after clear 1 (expect 0x80) | 0x80 | 0x80 | 0x80 |
| msip readback after set 2 (expect 1) | 1 | 1 | 1 |
| mip after set 2 (expect 0x88) | 0x88 | 0x88 | 0x88 |
| msip readback after clear 2 (expect 0) | 0 | 0 | 0 |
| mip after clear 2 (expect 0x80) | 0x80 | 0x80 | 0x80 |
| `mie.MSIE` / `mstatus.MIE` at end | 0 / 0 | 0 / 0 | 0 / 0 |
| QEMU exit code | 0 | 0 | 0 |
| RESULT | PASS | PASS | PASS |

What each value means:

- `mip = 0x80 -> 0x88 -> 0x80 -> 0x88 -> 0x80`: exactly bit 3
  transitions with the msip write/clear, on both cycles. The `0x80`
  remainder is the MTIP pending bit (mtime running past a zeroed
  mtimecmp), verified per-run against the live mtime/mtimecmp
  registers, and it never changed: the MSIP transition is the only
  movement in the register.
- `msip-readback` 1/0 on every transition: the register takes the
  write, so the bit 3 transition cannot be attributed to a stale or
  write-ignored register.
- `mie.MSIE = 0` / `mstatus.MIE = 0` at boot and end, with no trap
  handler linked: no trap was possible, so the sequence proves the
  pending-bit wiring specifically, distinct from the delivery path
  covered by `src/msip/` (backlog item 70), which verified that the
  same register write, with the interrupt enabled, delivers
  mcause `0x8000000000000003`.
- The only value that differs between runs is `mtime` at first read:
  a host-driven free-running counter, expected to vary; every
  check-relevant value is identical across all three runs.
- QEMU exit code 0 on all runs: the finisher shutdown path executed,
  i.e. `RESULT: PASS` with no parked FAIL.

## Toolchain note (measured, not assumed)

The module was first compiled with `/usr/bin/riscv64-unknown-elf-gcc`
13.2.0. Mid-run that compiler disappeared from the machine
(`/usr/bin/riscv64-unknown-elf-gcc` gone, `/usr/lib/gcc/riscv64-*`
gone; no other riscv64-unknown-elf toolchain on PATH). The repo
already vendored a replacement: xPack GNU RISC-V Embedded GCC 15.2.0
at `~/workspace/toolchains/xpack-riscv-none-elf-gcc-15.2.0-1`. The
final binary and all three runs above were built with
`riscv-none-elf-gcc` 15.2.0 using the unchanged Makefile flags
(`-Wall -Wextra -O2 -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`;
`make mip-msip.elf CROSS=riscv-none-elf-` with the xPack bin dir on
PATH). The build log in `bench-logs/build.log` records the exact
command. `boot.o` in the link was built earlier by the 13.2.0
toolchain; the linked flat image is the one that ran.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's CLINT model on the `virt` machine, not
  real silicon. The msip-to-MSIP-pending wiring is architectural, but
  the 4-byte-only msip access width and the zero-reset mtimecmp are
  this model's implementation details, verified empirically above.
- Only hart 0, only M-mode, only the software-interrupt pending bit.
  Actual interrupt delivery is the sibling module `src/msip/`; timer
  interrupt delivery, S-mode delegation, and multi-hart IPIs are not
  tested here; the module is deliberately that small.
- `mtime` at first read is host-driven and varies run to run; it is
  reported as data, not as a check value.

## Reproduction

```
make mip-msip.elf CROSS=riscv-none-elf-   # xPack bin dir on PATH
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mip-msip.elf
```

(Plain `make mip-msip.elf` once a `riscv64-unknown-elf-gcc` is back
on PATH.) QEMU 8.2.2, linked flat at 0x80000000 via `link.ld`.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`.
Each ends with `RESULT: PASS` and the finisher shutdown (exit 0);
the run logs were captured under `timeout` as with the other modules.
