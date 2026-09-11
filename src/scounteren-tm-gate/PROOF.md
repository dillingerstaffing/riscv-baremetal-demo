<!-- PROOF-HEADER
Checks: 18
Mismatches: 0
Checksum: 0x9756843e0befd207
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: scounteren.TM gates U-mode rdtime with an illegal-instruction trap

## What was built

`src/scounteren-tm-gate/`: a bare-metal RISC-V program that verifies, on
the QEMU `virt` board, the exact gating behavior of the `scounteren.TM`
bit for U-mode: with TM clear, a U-mode `rdtime` raises an
illegal-instruction exception instead of returning the timer; with TM
set, the same U-mode `rdtime` succeeds and returns an advancing count.
The gate only applies below M-mode, so the experiment performs two
real privilege drops (M -> S -> U, one per phase) to observe it. The
module shares only `src/boot.S` and `src/uart.c` with the other demos.

- `sctg_main.c`: M-mode setup (boot `scounteren` readback, 0x7
  writability probe, gate write of 0, `mcounteren.TM` set so the
  M-level gate does not mask the S-level gate under test,
  `mtvec`/`stvec`/`sscratch` install, `medeleg` bits 2 and 8 so the
  illegal-instruction trap and the U-mode `ecall` are delivered to
  S-mode while all other traps stay in M-mode, a
  whole-address-space PMP NAPOT entry, then `mret` with MPP=01 into
  the S-mode driver). The S-mode driver runs phase A (`sret` with
  SPP=0 into the U-mode payload A with TM clear, expecting exactly
  two S-mode traps: the gated `rdtime` with `scause = 2` and `sepc`
  exactly at the rdtime site, then the payload's `ecall` signal),
  then sets `scounteren.TM` itself and runs phase B (`sret` into
  the U-mode payload B, expecting no new trap from either
  `rdtime` and two strictly increasing samples delivered in the
  interrupted t0/t1 of the closing `ecall`). A 64-bit FNV-1a
  checksum is fed the eighteen verdict-relevant values in a fixed
  order from every privilege level and printed on the completion
  path. Only run-invariant values are fed (causes, counts, check
  booleans), never raw timer samples, so the checksum is identical
  on every passing run.
- `sctg_trap.S`: S-mode trap entry (direct mode). Records
  `scause`/`stval`/`sepc` and the interrupted t0/a0/t1/s0, appends
  each trap to an 8-entry history, and calls the C dispatcher
  `sctg_handle`, which returns the resume pc: `sepc+4` for the
  gated `rdtime` (exact: `rdtime` has no compressed encoding), or
  an S-mode continuation for the U-mode `ecall` signals (setting
  `sstatus.SPP=1` first so `sret` resumes in S-mode). Also holds
  the M-mode park handler (any trap reaching M-mode is
  unexpected) and both U-mode payloads with assembler-resolved
  site labels (`sctg_u_rdtime_a`, `sctg_u_ecall_a`,
  `sctg_u_rdtime_b`, `sctg_u_ecall_b`); the C `&&label` construct
  is never used for trap-resume addresses.
- `PROOF.md` (this file), `bench-logs/` with the build log, three
  raw QEMU run logs, and `host-cpu.txt`.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 18 checks held. On PASS the machine is
shut down through the virt test-device finisher (QEMU exits 0); on
FAIL the hart parks in a `wfi` loop without touching the finisher.

Build: direct `riscv64-unknown-elf-gcc` invocations matching the
repo Makefile pattern (`-Wall -Wextra -O2 -ffreestanding
-nostdlib -nostartfiles -no-pie -fno-pie -fno-pic
-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`), logged in
`bench-logs/build.log`.
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel scounteren-tm-gate.elf` under `timeout` so a parked-hart FAIL
is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the experiment drops to S-mode via
  `mret` with `mstatus.MPP = 01`, then to U-mode via `sret` with
  `sstatus.SPP = 0`.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`), xpack
  `riscv64-unknown-elf-gcc` 15.2.0, `-march=rv64imac_zicsr`.
- `medeleg = 0x104` (illegal-instruction trap and U-mode `ecall`
  delegated to S-mode); all other traps stay in M-mode and park.

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs exited 0 via the test-device finisher. The three logs are
byte-identical except for the absolute `rdtime` sample values
(which are a live timer); every verdict-relevant line is identical
across runs.

| step | operation | measured readback |
|---|---|---|
| boot | `csrr scounteren` | 0x0 on all 3 runs |
| probe | `csrw scounteren, 0x7`; readback | 0x7 on all 3 runs |
| gate | `csrw scounteren, 0`; readback | 0x0 on all 3 runs |
| unblock | `csrw mcounteren, 0x2` (TM); readback | 0x2 on all 3 runs |
| setup | `medeleg` readback | 0x104 on all 3 runs |
| phase A | U-mode `rdtime` at 0x80000254 with `scounteren.TM = 0` | trap, `scause = 0x2`, `stval = 0xc01022f3`, `sepc = 0x80000254` (exactly the rdtime site), interrupted t0 still the 0xdeadbeefdeadbeef sentinel |
| phase A | U-mode `ecall` at 0x80000260 (SIG_A) | trap, `scause = 0x8`, `sepc = 0x80000260` (exactly the payload ecall site), a0 = 0xa5a5; trap count 2 |
| phase B | `csrs scounteren, TM` in S-mode; readback | 0x2 on all 3 runs |
| phase B | U-mode `rdtime` x2 with `scounteren.TM = 1` | no new trap from either read; closing `ecall` (SIG_B) at 0x80000290 with `scause = 0x8`, a0 = 0xb5b5; samples strictly increasing: 0xe3d27 -> 0xe3d60 (delta 57), 0x7f048 -> 0x7f08a (delta 66), 0x495b2 -> 0x495ef (delta 61); trap count 3 |

Checks: 18 (scounteren probe readback 0x7, scounteren zero
readback, `mcounteren.TM` set, `medeleg` bits 2 and 8, phase-A
trap count 2, trap 1 `scause` 2, trap 1 `sepc` at the rdtime site,
trap 1 t0 sentinel intact, trap 2 `scause` 8, trap 2 `sepc` at the
payload ecall site, trap 2 signal SIG_A, `scounteren.TM` set
readback, phase-B trap count 3, trap 3 `scause` 8, trap 3 `sepc`
at the payload ecall site, trap 3 signal SIG_B, samples strictly
increasing, sample 0 nonzero). Mismatches: 0.
FNV-1a checksum over the eighteen verdict-relevant values:
0x9756843e0befd207, identical on all 3 runs.

Note: `stval = 0xc01022f3` is QEMU's informational readback of the
faulting `rdtime t0` encoding for the illegal-instruction trap; it
is printed as observed and is not part of the verdict or the
checksum.

## Limits of verification

- The +4 `sepc` advance in the S-mode handler is exact only
  because `rdtime` has no compressed encoding; the module asserts
  the result (`sepc` exactly at the labeled rdtime site, and the
  resume landing exactly on the labeled `ecall`, whose own trap
  record confirms it) rather than assuming it.
- The gate is exercised for U-mode only; S-mode `rdtime` gating
  by `scounteren.TM` was not tested here.
- QEMU-specific readbacks observed but not asserted: boot
  `scounteren = 0x0` and the `stval` encoding above.
- Payload B re-reads the second sample in a bounded spin
  (2^20 iterations) until it differs from the first, because
  back-to-back reads can land in the same timer tick; the bound
  never tripped on any run, and a trip would have signaled
  SIG_B_TIMEOUT, which the dispatcher routes to the FAIL path.
- No restore of the CSRs is performed: the machine halts on the
  completion path.
