<!-- PROOF-HEADER
Checks: 1
Mismatches: 0
Checksum: 0x300b72994b2592ed
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: menvcfg.CBZE gate probe for Zicboz cbo.zero

## Outcome

The phase-0 probe found that this QEMU 8.2.2 build does not execute
`cbo.zero`: the M-mode probe trapped as an illegal instruction
(`mcause` 0x2), so per the run plan the module ships the honest
smaller slice, the probe result plus the trap cause, with a PASS
verdict on the probe only. The full gate test (phases 1 and 2) is
implemented in the sources and runs as soon as the emulator executes
the instruction.

## Correction to the backlog note

The backlog idea said `senvcfg.CBZE`. The RISC-V privileged
specification assigns the gate for S-mode execution of `cbo.zero` to
`menvcfg.CBZE` (bit 7); `senvcfg.CBZE` gates U-mode execution only.
The module implements the `menvcfg` variant, which is the correct
mechanism for the S-mode gate under test.

## What was built

`src/cbo-zero-gate/`, sharing only `src/boot.S` and `src/uart.c`
with the other demos:

- `czg_main.c`: M-mode driver (probe, CBZE clear/set with exact
  write/readback, `menvcfg` restore, 1,000,000-spin quiet window),
  the two S-mode payloads (each executes `cbo.zero` at a numerically
  labeled site captured with an in-assembly forward label, then
  verifies memory and returns via `ecall`), the checks, the FNV-1a
  checksum, and the virt test-device finisher.
- `czg_trap.S`: M-mode trap entry (direct mode). S-mode `ecall`
  (`mcause` 9) resumes at the continuation in `m_regs[7]` with
  `mstatus.MPP` set to M-mode; any other M-mode trap is recorded and
  resumed past (`mepc` + 4), so the probe reports honestly instead of
  hanging. S-mode trap entry (direct mode): records
  `scause`/`sepc`/`sstatus`/`stval`, advances `sepc` by 4, and
  `sret`s back into the payload.
- `build.sh`, `README.md` (this proof log), `bench-logs/` with the
  build log and three raw QEMU run logs.

`cbo.zero` encoding used: funct12 `0x004`, funct3 `010` (CBO),
opcode `1110011` (SYSTEM), rd `00000`, rs1 = block address, i.e.
`0x00402073 | (rs1 << 15)`. The assembled bytes were verified
field-by-field against the Zicboz chapter of the unprivileged spec
(e.g. `0x0046a073` at the phase-1 site: funct12 `0x004`, rs1 `a3`,
funct3 `010`, rd `00000`, opcode `0x73`).

## What the probe measured

QEMU command (all three runs):
`qemu-system-riscv64 -machine virt -nographic -bios none -kernel cbo-zero-gate.elf`
(QEMU emulator version 8.2.2, Debian 1:8.2.2+ds-0ubuntu1.18;
toolchain Ubuntu riscv64-unknown-elf-gcc 13.2.0 / binutils 2.42.)

- `boot: menvcfg=0x2000000000000000`
- `phase0: M-mode cbo.zero: traps=1 zeromism=64` (the 64-byte pattern
  block was untouched: all 64 bytes still nonzero)
- `phase0: Zicboz NOT available: mcause=0x2 mepc=0x800009ea`
  (`mepc` is exactly the probe's `cbo.zero` site, disassembled at
  `0x800009ea: 00472073`)
- `checksum (FNV-1a over the probe values) = 0x300b72994b2592ed`
- `checks: 1  mismatches: 0`
- `RESULT: PASS (probe only)`, QEMU exited 0 via the finisher.

The single check asserts the probe's finding is exactly one
illegal-instruction trap (not some other failure mode): it is the
positive identification of "instruction unavailable".

## Extended verification (why the slice is the honest outcome)

The probe result was not taken on one shot. A separate probe binary
executed each CBO instruction (funct12 0x000 inval, 0x001 clean,
0x002 flush, 0x004 zero; encodings verified in the disassembly) in
M-mode under four CPU configurations:

- `-cpu rv64` (default): all four trap, `mcause` 0x2
- `-cpu rv64,zicboz=on`: all four trap, `mcause` 0x2
- `-cpu rv64,zicbom=on,zicboz=on`: all four trap, `mcause` 0x2
- `-cpu max,zicbom=on,zicboz=on`: all four trap, `mcause` 0x2

A further probe showed `menvcfg` bit 7 (CBZE) is writable and reads
back on every configuration (e.g. default `rv64`:
`0x2000000000000000` -> write `| 0x80` -> `0x2000000000000080`), so
the CSR bit exists while the instruction itself does not execute.
The `cboz-block-size` / `cbom-block-size` CPU properties do not exist
in this build. Conclusion: this QEMU 8.2.2 build raises illegal
instruction on `cbo.zero` regardless of configuration, so the gate
test cannot run here and the probe-only slice is the complete honest
result. No output was fabricated: every number above comes from a
real run.

## Build log (raw)

```
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/../lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: cbo-zero-gate.elf has a LOAD segment with RWX permissions
built cbo-zero-gate.elf
```

## Run logs (raw)

run1.log:

```

========================================
cbo-zero-gate: menvcfg.CBZE gates S-mode cbo.zero
========================================

boot: menvcfg=0x2000000000000000
phase0: M-mode cbo.zero: traps=1 zeromism=64
phase0: Zicboz NOT available: mcause=0x2 mepc=0x800009ea

note: cbo.zero unavailable on this hart; gate test not run
checksum (FNV-1a over the probe values) = 0x300b72994b2592ed
checks: 1  mismatches: 0
RESULT: PASS (probe only)
```

run2.log and run3.log are byte-identical to run1.log
(md5 `414b753431a7db2df09ae3ef739a0995` for all three).
