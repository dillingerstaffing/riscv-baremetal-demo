# cbo-zero-gate

A bare-metal RISC-V module probing the `menvcfg.CBZE` (bit 7) gate for the
Zicboz `cbo.zero` instruction on the QEMU `virt` board.

## What it does

`menvcfg.CBZE` is the M-mode gate that controls whether `cbo.zero` may
execute in S-mode: with CBZE=0 an S-mode `cbo.zero` must trap as an
illegal instruction, with CBZE=1 it zeroes a full cache block. (The
backlog note for this item said `senvcfg.CBZE`; the privileged spec
assigns the S-mode gate to `menvcfg.CBZE`, while `senvcfg.CBZE` gates
U-mode only. The module implements the `menvcfg` variant; the
correction is documented in `PROOF.md`.)

The run plan:

- **Phase 0 (M-mode):** probe Zicboz by executing `cbo.zero` on a
  64-byte scratch block, where no gate applies. Zero traps expected.
- **Phase 1:** clear `menvcfg.CBZE` (boot value read, bit 7 cleared,
  readback published), drop to S-mode, execute `cbo.zero` at a
  numerically labeled site. Exactly one trap expected: `scause` 0x2,
  `sepc` exactly at the site, block byte-identical to its snapshot.
- **Phase 2:** set `menvcfg.CBZE` (readback published), drop to
  S-mode, execute `cbo.zero` on a nonzero pattern. Zero traps
  expected; the full 64-byte block must read zero while the 16 guard
  bytes on each side keep their pattern.
- Then `menvcfg` is restored to its exact boot value and a
  1,000,000-spin quiet window must take zero traps.

## What actually happened

The phase-0 probe trapped: this QEMU 8.2.2 build raises illegal
instruction (`mcause` 0x2) on `cbo.zero` in M-mode under every CPU
configuration tried (default `rv64`, `rv64,zicboz=on`,
`rv64,zicbom=on,zicboz=on`, and `max`), and on all four CBO
instructions. So the module ships the honest smaller slice: the probe
result plus the trap cause, with a PASS verdict on the probe only.
The full gate test (phases 1 and 2) is implemented in the sources and
runs as soon as the emulator executes the instruction. See
`PROOF.md` for the complete investigation.

## Files

- `czg_main.c`: M-mode driver, S-mode payloads, checks, checksum, finisher.
- `czg_trap.S`: M-mode and S-mode trap entries (direct mode).
- `build.sh`: builds `cbo-zero-gate.elf` from the repo root.
- `PROOF.md`: the proof log with the machine-readable header.
- `bench-logs/`: build log and three raw QEMU run logs.

## Build and run

From this directory:

```
./build.sh > bench-logs/build.log 2>&1
qemu-system-riscv64 -machine virt -nographic -bios none \
  -kernel ../../cbo-zero-gate.elf
```

On PASS the machine shuts down through the virt test-device finisher
(QEMU exits 0); on FAIL the hart parks in a `wfi` loop (exit 124
under `timeout`). The module shares only `src/boot.S` and
`src/uart.c` with the other demos.
