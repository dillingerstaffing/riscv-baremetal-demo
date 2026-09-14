<!-- PROOF-HEADER
Checks: 14
Mismatches: 0
Checksum: 0x0edf2feb40411329
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: stval WARL write probe in S-mode

## What was built

`src/stval-warl-probe/`: a bare-metal RISC-V program that
verifies, on the QEMU `virt` board, that `stval` is WARL in the
sense the privileged spec defines: a write of all-ones
legalizes to the value the hart permits (on this hart the write
is stored verbatim, readback `0xffffffffffffffff`), a write of
zero reads back `0x0`, and the boot value restores exactly.
Between the writes the module takes one deliberate
illegal-instruction trap (`csrr t0, 0x7ff`, an unimplemented
CSR) so the run also records what the hart writes into `stval`
on a trap: the S-mode handler captures `scause`/`stval`/`sepc`,
and the verdict requires `scause == 2`, `sepc` exactly at the
fault site, `stval` exactly equal to the faulting instruction
word read back from the site address, and the destination
register still holding its pre-fault sentinel (the faulting
instruction never retired). The module shares only
`src/boot.S` and `src/uart.c` with the other demos.

- `stv_main.c`: M-mode setup (boot `stval` readback, `mtvec` /
  `stvec` install, `mscratch` pointed at the M-mode record,
  `sscratch` at the S-mode record, `medeleg = 0x4` so the
  illegal-instruction trap is delivered to S-mode while the
  final ecall stays in M-mode, a whole-address-space PMP NAPOT
  entry, then `mret` with MPP=01 into the S-mode payload). The
  S-mode payload runs phase 1 (boot `stval` readback, expect
  `0x0`), phase 2 (write all-ones, expect the verbatim readback
  `0xffffffffffffffff`), phase 3 (write zero, expect `0x0`),
  phase 4 (the labeled `csrr t0, 0x7ff` fault: expect exactly
  one S-mode trap with `scause = 2`, `sepc` exactly at the fault
  site, the handler's +4 advance landing on the labeled resume
  address, `stval` exactly equal to the faulting instruction
  word read from the site address, and t0 still holding the
  `0xdeadbeefdeadbeef` sentinel), phase 5 (restore the boot
  value, expect the boot value back), and phase 6 (a labeled
  `ecall` back to M-mode). The fault site addresses use in-asm
  numeric local labels (`la t, 1f` with `1:` in the asm) so the
  assembler resolves them exactly; the C `&&label` construct is
  never used for trap-resume addresses. The ecall site address
  is stored to its global inside the asm before the ecall
  executes, because the M-mode handler never returns and a
  compiler-scheduled store after the asm would never run. A
  64-bit FNV-1a checksum is fed the fourteen verdict-relevant
  values in a fixed order from both privilege levels (booleans
  for the address comparisons, raw values elsewhere; nothing
  timing-related exists in this module) and printed on the
  completion path.
- `stv_strap.S`: S-mode trap entry (direct mode). Records
  `scause`/`stval`/`sepc`, bumps the trap counter, advances
  `sepc` by 4 (exact: the SYSTEM instruction `csrr t0, 0x7ff`
  has no compressed encoding, so the faulting instruction is
  always 4 bytes), and `sret`.
- `stv_mtrap.S`: M-mode trap entry (direct mode). On the one
  expected ecall from S-mode it records `mcause`/`mepc` and the
  trap count, advances `mepc` past the ecall, and calls the C
  verdict routine `stv_ecall_done`, which performs the M-mode
  checks, prints the checksum and `RESULT`, and then either
  shuts the machine down through the virt test-device finisher
  (PASS, QEMU exits 0) or parks the hart in `wfi` (FAIL). Any
  other M-mode trap parks the hart immediately.
- `build.sh`, `PROOF.md` (this file), `bench-logs/` with the
  build log and three raw QEMU run logs.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 14 checks held. On PASS the machine is
shut down through the virt test-device finisher (QEMU exits 0);
on FAIL the hart parks in a `wfi` loop without touching the
finisher.

Build: direct `riscv64-unknown-elf-gcc` invocations matching the
repo Makefile pattern (`-Wall -Wextra -O2 -ffreestanding
-nostdlib -nostartfiles -no-pie -fno-pie -fno-pic
-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`), logged in
`bench-logs/build.log` via `build.sh`; the Makefile itself was not
modified.
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel stval-warl-probe.elf` under `timeout`, so a parked-hart
FAIL is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the experiment drops to S-mode via
  `mret` with `mstatus.MPP = 01`, then takes one M-mode excursion
  via `ecall` for the verdict.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`),
  `riscv64-unknown-elf-gcc` 13.2.0, `-march=rv64imac_zicsr`.
- `medeleg = 0x4` (only the illegal-instruction trap delegated to
  S-mode; the final ecall is deliberately kept in M-mode).

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs exited 0 via the test-device finisher. The three logs are
byte-identical: every value in this module is deterministic, so
there is no run-to-run variation at all.

| step | operation | measured readback |
|---|---|---|
| boot | `csrr stval` in M-mode | 0x0 on all 3 runs |
| setup | `medeleg` readback | 0x4 on all 3 runs |
| phase 1 | S-mode `csrr stval` | 0x0 on all 3 runs |
| phase 2 | `csrw stval, 0xffffffffffffffff`; readback | 0xffffffffffffffff on all 3 runs (stored verbatim) |
| phase 3 | `csrw stval, 0x0`; readback | 0x0 on all 3 runs |
| phase 4 | S-mode `csrr t0, 0x7ff` at 0x80000486 | trap, `scause = 0x2`, `stval = 0x7ff022f3`, `sepc = 0x80000486` (exactly the fault site), `sepc+4 = 0x8000048a` (the resume label), trap count 1, faulting instruction word read from the site address = 0x7ff022f3 (exactly equal to `stval`), t0 still the 0xdeadbeefdeadbeef sentinel |
| phase 5 | `csrw stval, 0x0` (boot value); readback | 0x0 on all 3 runs |
| phase 6 | S-mode `ecall` at 0x80000648, handled in M-mode | `mcause = 0x9`, `mepc = 0x80000648` (exactly the ecall site), M-mode trap count 1 |

Checks: 14 (boot stval == 0x0, all-ones readback verbatim,
zero readback == 0x0, trap count == 1, scause == 2,
sepc == fault site, sepc+4 == resume label, stval ==
faulting instruction word, t0 sentinel intact, restore
readback == boot value, medeleg bit 2, M-mode trap count == 1,
mcause == 9, mepc == ecall site). Mismatches: 0.
FNV-1a checksum over the fourteen verdict-relevant values:
0x0edf2feb40411329, identical on all 3 runs.

Note: `0x7ff022f3` is the assembled encoding of
`csrr t0, 0x7ff` (that is, `csrrs t0, x0, 0x7ff`: csr 0x7ff,
rs1 x0, funct3 010, rd x5, opcode 1110011). The module does not
hard-code it: the verdict reads the instruction word from the
fault-site address and requires `stval` to equal that word, so
the equality is measured, not assumed.

## Limits of verification

- The verbatim all-ones readback is this hart's legalization
  choice (QEMU 8.2.2 stores `stval` without masking); the spec
  permits a hart to legalize to something else, so the portable
  claim is only that the readback is deterministic and
  published, not that every hart stores all-ones.
- The +4 `sepc` advance in the S-mode handler is exact only
  because the faulting SYSTEM instruction has no compressed
  encoding; the module asserts the result (`sepc+4` equals the
  labeled resume address) rather than assuming it.
- Only the illegal-instruction trap's `stval` capture is
  exercised here; other trap causes' `stval` values were covered
  by `src/stval-illegal-capture` and `src/stval-ecall-capture`.
- Emulator behavior, not silicon: every number above was measured
  under QEMU 8.2.2 `virt`, not on a physical hart.
