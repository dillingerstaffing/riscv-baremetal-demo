<!-- PROOF-HEADER
Checks: 34
Mismatches: 0
Checksum: 0x96e2802b2ebd9505
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mcounteren.TM gates U-mode rdtime with an illegal-instruction trap

## What was built

`src/mcounteren-tm-u-gate/`: a bare-metal RISC-V program that verifies, on
the QEMU `virt` board, the exact gating behavior of the `mcounteren.TM`
bit (bit 1; the backlog title's bit-0 note is a typo, bit 0 is CY) for
U-mode: with TM set, a U-mode `rdtime` succeeds and returns an advancing
count; with TM clear, the same U-mode `rdtime` raises an
illegal-instruction exception instead of returning the timer; with TM set
again, the read succeeds again. `scounteren.TM` is held set for the whole
run so the M-level gate is the only one under test. The experiment
performs three real privilege drops (M -> S -> U, one per phase) and, at
the end, restores `mcounteren`, `scounteren`, and `medeleg` to their boot
values before shutting the machine down. The module shares only
`src/boot.S` and `src/uart.c` with the other demos.

This is the M-level counterpart of the done `scounteren-tm-gate` module
(which held `mcounteren.TM` set and toggled the S-level gate) and of
`mcounteren-time-gate` (which gated S-mode reads); none of them observed
the M-level gate's effect on a U-mode read.

- `mtmu_main.c`: M-mode setup (boot `mcounteren`/`scounteren`/`medeleg`
  readbacks, 0x7 writability probe on `mcounteren`, gate writes of
  `mcounteren = 0x2` and `scounteren = 0x2`, `mtvec`/`stvec`/`sscratch`
  install, `medeleg` bits 2 and 8 so the illegal-instruction trap and
  the U-mode `ecall` are delivered to S-mode while the S-mode `ecall`
  (bit 9) stays in M-mode as the phase handoff, a whole-address-space
  PMP NAPOT entry, then `mret` with MPP=01 into the S-mode driver). The
  S-mode driver runs phase A (`sret` with SPP=0 into the U-mode payload
  A with TM set, expecting exactly one S-mode trap: the payload's
  `ecall` with two strictly increasing `rdtime` samples in the
  interrupted t0/t1), hands back to M-mode via an S-mode `ecall`,
  which clears `mcounteren.TM` and mrets into the phase-B driver;
  phase B (`sret` into the U-mode payload B, expecting exactly two
  more S-mode traps: the gated `rdtime` with `scause = 2` and `sepc`
  exactly at the rdtime site, then the payload's `ecall` signal);
  phase C (M-mode sets `mcounteren.TM` again, `sret` into the U-mode
  payload C, expecting one more S-mode trap: the payload's `ecall`
  with two strictly increasing samples). A 64-bit FNV-1a checksum is
  fed the thirty-four verdict-relevant values in a fixed order from
  every privilege level and printed on the completion path. Only
  run-invariant values are fed (causes, counts, check booleans),
  never raw timer samples, so the checksum is identical on every
  passing run.
- `mtmu_trap.S`: S-mode trap entry (direct mode). Records
  `scause`/`stval`/`sepc` and the interrupted t0/a0/t1/s0, appends
  each trap to an 8-entry history, and calls the C dispatcher
  `mtmu_handle`, which returns the resume pc: `sepc+4` for the
  gated `rdtime` (exact: `rdtime` has no compressed encoding), or
  an S-mode continuation for the U-mode `ecall` signals (setting
  `sstatus.SPP=1` first so `sret` resumes in S-mode). Also holds
  the M-mode trap entry (records `mcause`/`mepc`, bumps the
  M-mode trap counter, calls `mtmu_m_handle`, which arms the next
  phase or restores the CSRs and exits; it never returns to the
  interrupted context) and the three U-mode payloads with
  assembler-resolved site labels (`mtmu_u_rdtime_a/b/c`,
  `mtmu_u_ecall_a/b/c`); the C `&&label` construct is never used
  for trap-resume addresses.
- `PROOF.md` (this file), `bench-logs/` with the build log, three
  raw QEMU run logs, and `host-cpu.txt`.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 34 checks held. On PASS the CSRs are
restored to their boot values (readbacks published) and the machine
is shut down through the virt test-device finisher (QEMU exits 0);
on FAIL the hart parks in a `wfi` loop without touching the
finisher.

Build: direct `riscv64-unknown-elf-gcc` invocations matching the
repo Makefile pattern (`-Wall -Wextra -O2 -ffreestanding
-nostdlib -nostartfiles -no-pie -fno-pie -fno-pic
-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`), logged in
`bench-logs/build.log`.
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mcounteren-tm-u-gate.elf` under `timeout` so a parked-hart FAIL
is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the experiment drops to S-mode via
  `mret` with `mstatus.MPP = 01`, then to U-mode via `sret` with
  `sstatus.SPP = 0`.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`), Ubuntu
  `riscv64-unknown-elf-gcc` 13.2.0, `-march=rv64imac_zicsr`.
- `medeleg = 0x104` (illegal-instruction trap and U-mode `ecall`
  delegated to S-mode); the S-mode `ecall` stays in M-mode as the
  phase handoff; all other traps stay in M-mode and take the FAIL
  path.

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs exited 0 via the test-device finisher. The three logs are
byte-identical except for the absolute `rdtime` sample values
(which are a live timer); every verdict-relevant line is identical
across runs.

| step | operation | measured readback |
|---|---|---|
| boot | `csrr mcounteren` | 0x0 on all 3 runs |
| boot | `csrr scounteren` | 0x0 on all 3 runs |
| boot | `csrr medeleg` | 0x0 on all 3 runs |
| probe | `csrw mcounteren, 0x7`; readback | 0x7 on all 3 runs |
| arm | `csrw mcounteren, 0x2` (TM); readback | 0x2 on all 3 runs |
| arm | `csrw scounteren, 0x2` (TM); readback | 0x2 on all 3 runs |
| setup | `medeleg` readback | 0x104 on all 3 runs |
| phase A | U-mode `rdtime` x2 at 0x80000260 with `mcounteren.TM = 1` | no trap from either read; closing `ecall` (SIG_A) at 0x8000028c with `scause = 0x8`, a0 = 0xa1a1; samples strictly increasing: 0x173b2 -> 0x173e9 (delta 55), 0x35edd -> 0x35f13 (delta 54), 0x2957c -> 0x295b7 (delta 59); trap count 1 |
| M handoff | S-mode `ecall` after phase A | `mcause = 0x9`, `mepc` exactly at the S-mode ecall site, M-mode trap count 1; `scounteren` still 0x2; `mcounteren` cleared to 0x0 |
| phase B | U-mode `rdtime` at 0x800002b0 with `mcounteren.TM = 0` | trap, `scause = 0x2`, `stval = 0xc01022f3`, `sepc = 0x800002b0` (exactly the rdtime site), interrupted t0 still the 0xdeadbeefdeadbeef sentinel |
| phase B | U-mode `ecall` at 0x800002bc (SIG_B) | trap, `scause = 0x8`, `sepc = 0x800002bc` (exactly the payload ecall site), a0 = 0xa2a2; trap count 3 |
| M handoff | S-mode `ecall` after phase B | `mcause = 0x9`, `mepc` exactly at the S-mode ecall site, M-mode trap count 2; `scounteren` still 0x2; `mcounteren` set to 0x2 |
| phase C | U-mode `rdtime` x2 at 0x800002c0 with `mcounteren.TM = 1` | no trap from either read; closing `ecall` (SIG_C) at 0x800002ec with `scause = 0x8`, a0 = 0xa3a3; samples strictly increasing: 0x4309c -> 0x430ec (delta 80), 0x8fced -> 0x8fd2e (delta 65), 0x5745a -> 0x57493 (delta 57); trap count 4 |
| M handoff | S-mode `ecall` after phase C | `mcause = 0x9`, `mepc` exactly at the S-mode ecall site, M-mode trap count 3 |
| restore | `mcounteren` / `scounteren` / `medeleg` written to boot values | readbacks 0x0 / 0x0 / 0x0 on all 3 runs |

Checks: 34 (boot `mcounteren` 0, `mcounteren` 0x7 probe readback,
`mcounteren.TM` set, `scounteren.TM` set, `medeleg` bits 2 and 8,
phase-A trap count 1, trap 1 `scause` 8, trap 1 `sepc` at the
payload ecall site, trap 1 signal SIG_A, phase-A samples strictly
increasing, phase-A sample 0 nonzero, three M-mode handoffs each
with `mcause` 9 and `mepc` at the S-mode ecall site,
`scounteren.TM` still set at both phase handoffs, `mcounteren.TM`
cleared (0 readback) before phase B, `mcounteren.TM` set (0x2
readback) before phase C, phase-B trap count 3, trap 2 `scause` 2,
trap 2 `sepc` at the rdtime site, trap 2 t0 sentinel intact, trap
3 `scause` 8, trap 3 `sepc` at the payload ecall site, trap 3
signal SIG_B, phase-C trap count 4, trap 4 `scause` 8, trap 4
`sepc` at the payload ecall site, trap 4 signal SIG_C, phase-C
samples strictly increasing, phase-C sample 0 nonzero,
`mcounteren`/`scounteren`/`medeleg` restore readbacks equal to the
boot values, M-mode trap count exactly 3). Mismatches: 0.
FNV-1a checksum over the thirty-four verdict-relevant values:
0x96e2802b2ebd9505, identical on all 3 runs.

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
  by `mcounteren.TM` was already covered by `mcounteren-time-gate`
  and was not re-tested here.
- QEMU-specific readbacks observed but not asserted: boot
  `scounteren = 0x0`, boot `medeleg = 0x0`, and the `stval`
  encoding above.
- Payloads A and C re-read the second sample in a bounded spin
  (2^20 iterations) until it differs from the first, because
  back-to-back reads can land in the same timer tick; the bound
  never tripped on any run, and a trip would have signaled
  SIG_A_TIMEOUT / SIG_C_TIMEOUT, which the dispatcher routes to
  the FAIL path.
- The PMP NAPOT entry programmed during setup is left in place at
  exit: the machine halts through the test-device finisher from
  M-mode (whose accesses are not PMP-gated), so restoring it has
  no observable effect; `mcounteren`, `scounteren`, and `medeleg`
  (the CSRs the experiment changed) are all restored to their
  boot values with readbacks published.
