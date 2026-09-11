<!-- PROOF-HEADER
Checks: 17
Mismatches: 0
Checksum: 0xc1860c1ec97b7184
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mip reflects a pended interrupt while the global gate is off (backlog item 171)

## What was built

`src/mip-pending-no-trap/`: a bare-metal RISC-V program that verifies
the mip pending bit tracks the asserted state independently of the
interrupt enable gate on the QEMU `virt` board. Two files, sharing
only `src/boot.S` and `src/uart.c` with the other demos. Exactly one
mechanism is under test: with `mstatus.MIE` and `mie.MSIE` both clear,
setting the CLINT msip bit for hart 0 drives mip bit 3 (MSIP) to 1,
the bit stays 1 across a 100,000-mcycle polling window with zero
traps fired, and clearing msip drives the bit back to 0.

- `mpnt_trap.S`: M-mode trap entry that bumps a trap counter in the
  `mpnt_regs` array via mscratch and records mcause/mepc/mtval,
  advancing mepc past the trapping instruction and returning with
  mret. Both interrupt enables stay clear for the whole run, so the
  handler is expected never to fire; any entry is counted and the
  verdict requires the count to be 0, so a fired trap becomes visible
  instead of silent.
- `mpnt_main.c`: verifies the boot state (both enables clear,
  mip.MSIP clear), explicitly clears both enables via `csrc` and
  re-reads them, installs the direct-mode mtvec handler, sets msip
  (32-bit access, per the CLINT defect documented in
  `src/msip/PROOF.md`, backlog item 70) and requires mip.MSIP = 1,
  spins a polling window of at least 100,000 mcycle ticks with the
  bit asserted, requires mip.MSIP still 1 and the trap counter still
  0, clears msip and requires mip.MSIP = 0 again, then confirms both
  enables still clear and no trap ever fired. A failed check prints
  `FAIL` and flips the verdict; `RESULT: PASS` is printed only when
  every check held. The verdict-relevant values feed a 64-bit FNV-1a
  digest printed as the last data line, so the three bench runs can
  be compared for identical verdict data. On PASS the machine is shut
  down through the virt test-device finisher (QEMU exits 0); on FAIL
  the hart parks without touching the finisher.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make mip-pending-no-trap.elf` (added to `all` in the
Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel mip-pending-no-trap.elf`
(or `make run-mip-pending-no-trap`).

## Configuration under test

- Hart: hart 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- CLINT msip for hart 0 at `0x02000000`, accessed as a 32-bit
  register (this QEMU's CLINT model only accepts 4-byte accesses to
  msip; see the defect note in `src/msip/PROOF.md`, backlog item 70).
- `mie.MSIE = 0` and `mstatus.MIE = 0` throughout: read clear at
  boot, explicitly cleared again via `csrc` and re-read, re-read
  clear at the end. A direct-mode mtvec handler is installed and
  counting; the verdict requires its entry count to be 0.
- QEMU 8.2.2 `virt` machine, xPack `riscv-none-elf-gcc` 15.2.0
  (see the toolchain note below).

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

| step | run1 | run2 | run3 |
|---|---|---|---|
| `mie.MSIE` / `mstatus.MIE` at boot | 0 / 0 | 0 / 0 | 0 / 0 |
| mip at boot | 0x80 | 0x80 | 0x80 |
| msip readback after set (expect 1) | 1 | 1 | 1 |
| mip after set | 0x88 (MSIP=1) | 0x88 (MSIP=1) | 0x88 (MSIP=1) |
| window cycles (expect >= 100000) | 100050 | 100125 | 100080 |
| mip after window | 0x88 (MSIP=1) | 0x88 (MSIP=1) | 0x88 (MSIP=1) |
| traps after window (expect 0) | 0 | 0 | 0 |
| msip readback after clear (expect 0) | 0 | 0 | 0 |
| mip after clear | 0x80 (MSIP=0) | 0x80 (MSIP=0) | 0x80 (MSIP=0) |
| `mie.MSIE` / `mstatus.MIE` at end | 0 / 0 | 0 / 0 | 0 / 0 |
| traps at end (expect 0) | 0 | 0 | 0 |
| checks / mismatches | 17 / 0 | 17 / 0 | 17 / 0 |
| digest | 0xc1860c1ec97b7184 | 0xc1860c1ec97b7184 | 0xc1860c1ec97b7184 |
| QEMU exit code | 0 | 0 | 0 |
| RESULT | PASS | PASS | PASS |

What each value means:

- `mip = 0x80 -> 0x88 -> 0x88 -> 0x80`: exactly bit 3 transitions
  with the msip write/clear, and holds asserted across the
  100,000-cycle window. The `0x80` remainder is the MTIP pending bit
  (mtime running past a zeroed mtimecmp), conserved across the whole
  run. Bit 3 moving while bits 7 and everything else stay fixed is
  the observation that the pending bit is the CLINT msip's own
  reflection, not a trap side effect.
- `traps = 0` with both enables off: the handler was installed and
  counting, so the 0 is a measurement, not an absence of
  instrumentation. The pended bit showed in mip for more than
  100,000 cycles and no trap was taken; the enable gate, not the
  pending state, is what governs delivery.
- The spin-window cycle count varies slightly run to run (100050,
  100125, 100080): it is an mcycle poll overshoot driven by host
  timing, reported as data, not as a check value (the check is only
  `>= 100000`). The digest covers only the verdict-relevant values
  and is identical across all three runs.
- Distinct from the siblings: `src/msip/` (item 70) measured delivery
  latency with the interrupt enabled, `src/mie-toggle/` toggled MIE to
  deliver a pended timer interrupt, and `src/mip-msip/` (item 111)
  tracked the bit across set/clear cycles without a handler; this
  module is the only one that asserts the bit with the global gate
  off, holds it across a timed window, and requires the trap count
  to be 0.

## Toolchain note (measured, not assumed)

The repo's vendored xPack GNU RISC-V Embedded GCC 15.2.0 at
`~/workspace/toolchains/xpack-riscv-none-elf-gcc-15.2.0-1` was used:
`riscv-none-elf-gcc` with the unchanged Makefile flags (`-Wall
-Wextra -O2 -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`;
`make mip-pending-no-trap.elf CROSS=riscv-none-elf-` with the xPack
bin dir on PATH). The build log in `bench-logs/build.log` records
the exact command. `boot.o` in the link was built earlier by the
13.2.0 toolchain; the linked flat image is the one that ran. QEMU
is `~/workspace/qemu/usr/bin/qemu-system-riscv64` 8.2.2 (the
`~/workspace/toolchains/compat-bin` copy is broken on this VM:
missing libfdt/libfuse3).

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's CSR/CLINT model on the `virt` machine,
  not real silicon. That a pending bit can sit asserted for 100k+
  cycles without a trap while the enable gate is clear is the
  architectural contract, but the exact register readbacks and the
  4-byte-only msip access width are this model's implementation
  details, verified empirically above.
- Only hart 0, only M-mode, only the machine software-interrupt
  pending bit. Timer interrupt delivery, S-mode delegation, and
  multi-hart IPIs are not tested here; the module is deliberately
  that small.
- The window length is an mcycle count, not wall time; the exact
  overshoot varies with the host.

## Reproduction

```
make mip-pending-no-trap.elf CROSS=riscv-none-elf-   # xPack bin dir on PATH
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mip-pending-no-trap.elf
```

(Plain `make mip-pending-no-trap.elf` once a `riscv64-unknown-elf-gcc`
is back on PATH.) QEMU 8.2.2, linked flat at 0x80000000 via
`link.ld`. Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `run2.log`, `run3.log`. Each ends with
`RESULT: PASS` and the finisher shutdown (exit 0); the run logs were
captured under `timeout` as with the other modules.
