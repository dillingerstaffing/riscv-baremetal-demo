<!-- PROOF-HEADER
Checks: 22
Mismatches: 0
Checksum: 0x0f857e15c046194f
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mcounteren.CY gates U-mode rdcycle with an illegal-instruction trap

## What was built

`src/mcounteren-cy-u-read/`: a bare-metal RISC-V program that verifies, on
the QEMU `virt` board, the exact gating behavior of the `mcounteren.CY`
bit for U-mode: with CY set, a U-mode `rdcycle` succeeds and returns
strictly increasing samples; with CY clear, the same U-mode `rdcycle`
raises an illegal-instruction exception (`scause = 2`) instead of
returning the cycle count, and the destination register keeps its
pre-fault sentinel. The S-level gate (`scounteren.CY`) is held set for
the whole run so it can never mask the M-level gate under test; this
module is the converse of `src/scounteren-cy-gate`, which held the
M-level gate set and toggled the S-level one. The gate only applies
below M-mode, so the experiment performs two real privilege drops
(M -> S -> U, one per phase) to observe it. The module shares only
`src/boot.S` and `src/uart.c` with the other demos.

- `mcyu_main.c`: M-mode setup (boot `mcounteren` readback, 0x7
  writability probe, gate arm of `mcounteren.CY`, `scounteren.CY`
  set and held so the S-level gate never masks the M-level gate
  under test, `mtvec`/`stvec`/`sscratch` install, `medeleg` bits 2
  and 8 so the illegal-instruction trap and the U-mode `ecall` are
  delivered to S-mode while the S-mode `ecall` stays in M-mode as
  the phase handoff, a whole-address-space PMP NAPOT entry, then
  `mret` with MPP=01 into the S-mode driver). The S-mode driver
  runs phase A (`sret` with SPP=0 into the U-mode payload A with
  `mcounteren.CY=1`, expecting no trap from either `rdcycle` and
  two strictly increasing samples delivered in the interrupted
  t0/t1 of the closing `ecall`), then issues an S-mode `ecall`
  (cause 9, not delegated) to hand back to M-mode. The M-mode
  handler verifies the handoff (`mcause = 9`, `mepc` exactly at
  the S-mode ecall site), requires `scounteren` to still read
  0x1, clears `mcounteren.CY`, requires the 0 readback, and
  `mret`s with MPP=01 into the phase-B driver, which `sret`s
  with SPP=0 into the U-mode payload B (expecting exactly two
  more S-mode traps: the gated `rdcycle` with `scause = 2` and
  `sepc` exactly at the rdcycle site and the destination still
  holding its sentinel, then the payload's `ecall` signal). A
  64-bit FNV-1a checksum is fed the twenty-two verdict-relevant
  values in a fixed order from every privilege level and printed
  on the completion path. Only run-invariant values are fed
  (causes, counts, check booleans), never raw cycle samples, so
  the checksum is identical on every passing run.
- `mcyu_trap.S`: S-mode trap entry (direct mode). Records
  `scause`/`stval`/`sepc` and the interrupted t0/a0/t1/s0, appends
  each trap to an 8-entry history, and calls the C dispatcher
  `mcyu_handle`, which returns the resume pc: `sepc+4` for the
  gated `rdcycle` (exact: `rdcycle` has no compressed encoding),
  or an S-mode continuation for the U-mode `ecall` signals
  (setting `sstatus.SPP=1` first so `sret` resumes in S-mode).
  Also holds the M-mode trap entry (records `mcause`/`mepc` and
  calls the C handler `mcyu_m_handle`, which arms phase B and
  `mret`s; it never returns to the entry), the labeled S-mode
  ecall handoff `mcyu_s_ecall` (so the M-mode handler can check
  `mepc` against `mcyu_s_ecall_site` exactly), and both U-mode
  payloads with assembler-resolved site labels
  (`mcyu_u_rdcycle_a`, `mcyu_u_ecall_a`, `mcyu_u_rdcycle_b`,
  `mcyu_u_ecall_b`); the C `&&label` construct is never used for
  trap-resume addresses.
- `PROOF.md` (this file), `bench-logs/` with the build log, three
  raw QEMU run logs, and `host-cpu.txt`.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 22 checks held. On PASS the machine is
shut down through the virt test-device finisher (QEMU exits 0); on
FAIL the hart parks in a `wfi` loop without touching the finisher.

Build: `make mcounteren-cy-u-read.elf` with the repo Makefile
(`-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie
-fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`),
logged in `bench-logs/build.log`.
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mcounteren-cy-u-read.elf` under `timeout` so a parked-hart FAIL
is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the experiment drops to S-mode via
  `mret` with `mstatus.MPP = 01`, then to U-mode via `sret` with
  `sstatus.SPP = 0`.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`),
  `riscv64-unknown-elf-gcc` 13.2.0 (Debian), `-march=rv64imac_zicsr`.
- `medeleg = 0x104` (illegal-instruction trap and U-mode `ecall`
  delegated to S-mode); the S-mode `ecall` stays in M-mode as the
  phase handoff; all other traps take the FAIL path.

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs exited 0 via the test-device finisher. The three logs are
byte-identical except for the absolute `rdcycle` sample values
(which are a live counter) and their derived deltas; every
verdict-relevant line is identical across runs.

| step | operation | measured readback |
|---|---|---|
| boot | `csrr mcounteren` | 0x0 on all 3 runs |
| probe | `csrw mcounteren, 0x7`; readback | 0x7 on all 3 runs |
| arm | `csrw mcounteren, 0x1` (CY); readback | 0x1 on all 3 runs |
| hold | `csrw scounteren, 0x1` (CY); readback | 0x1 on all 3 runs |
| setup | `medeleg` readback | 0x104 on all 3 runs |
| phase A | U-mode `rdcycle` x2 at 0x80000254 with `mcounteren.CY = 1` | no trap from either read; closing `ecall` (SIG_A) at 0x80000280 with `scause = 0x8`, a0 = 0x9a9a; samples strictly increasing: 0x255b42dc37b -> 0x255b42de27a (delta 7935), 0x255b78faea3 -> 0x255b78fcd75 (delta 7890), 0x255bc69138d -> 0x255bc693322 (delta 8085); trap count 1 |
| handoff | S-mode `ecall` between phases | M-mode trap, `mcause = 0x9`, `mepc = 0x8000024c` (exactly the S-mode ecall site), `scounteren` still 0x1, `mcounteren` after clear 0x0 |
| phase B | U-mode `rdcycle` at 0x800002a4 with `mcounteren.CY = 0` | trap, `scause = 0x2`, `stval = 0xc00022f3`, `sepc = 0x800002a4` (exactly the rdcycle site), interrupted t0 still the 0xdeadbeefdeadbeef sentinel |
| phase B | U-mode `ecall` at 0x800002b0 (SIG_B) | trap, `scause = 0x8`, `sepc = 0x800002b0` (exactly the payload ecall site), a0 = 0x9b9b; trap count 3 |

Checks: 22 (boot `mcounteren` readback 0x0, `mcounteren` probe
readback 0x7, `mcounteren.CY` set, `scounteren.CY` set,
`medeleg` bits 2 and 8, phase-A trap count 1, trap 1 `scause`
8, trap 1 `sepc` at the payload ecall site, trap 1 signal
SIG_A, samples strictly increasing, sample 0 nonzero, handoff
`mcause` 9, handoff `mepc` at the S-mode ecall site,
`scounteren` still 0x1, `mcounteren` cleared to 0x0, phase-B
trap count 3, trap 2 `scause` 2, trap 2 `sepc` at the rdcycle
site, trap 2 t0 sentinel intact, trap 3 `scause` 8, trap 3
`sepc` at the payload ecall site, trap 3 signal SIG_B).
Mismatches: 0.
FNV-1a checksum over the twenty-two verdict-relevant values:
0x0f857e15c046194f, identical on all 3 runs.

Note: `stval = 0xc00022f3` is QEMU's informational readback of the
faulting `rdcycle t0` (`csrrs t0, cycle, x0`) encoding for the
illegal-instruction trap; it is printed as observed and is not
part of the verdict or the checksum.

## Limits of verification

- The +4 `sepc` advance in the S-mode handler is exact only
  because `rdcycle` has no compressed encoding; the module asserts
  the result (`sepc` exactly at the labeled rdcycle site, and the
  resume landing exactly on the labeled `ecall`, whose own trap
  record confirms it) rather than assuming it.
- The gate is exercised for U-mode only; S-mode `rdcycle`
  gating by `mcounteren.CY` was not tested here (that is the done
  `mcounteren-cy-gate` module's S-mode case).
- QEMU-specific readbacks observed and asserted: boot
  `mcounteren = 0x0` and the `stval` encoding above (printed, not
  asserted).
- Payload A re-reads the second sample in a bounded spin
  (2^20 iterations) until it differs from the first, because
  back-to-back reads can land in the same host tick; the bound
  never tripped on any run, and a trip would have signaled
  SIG_A_TIMEOUT, which the dispatcher routes to the FAIL path.
- No restore of the CSRs is performed: the machine halts on the
  completion path.
