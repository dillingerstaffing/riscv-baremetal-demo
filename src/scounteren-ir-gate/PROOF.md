<!-- PROOF-HEADER
Checks: 18
Mismatches: 0
Checksum: 0x9756843e0befd207
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: scounteren.IR gates U-mode rdinstret with an illegal-instruction trap

## What was built

`src/scounteren-ir-gate/`: a bare-metal RISC-V program that verifies, on
the QEMU `virt` board, the exact gating behavior of the `scounteren.IR`
bit for U-mode: with IR clear, a U-mode `rdinstret` raises an
illegal-instruction exception instead of returning the
retired-instruction count; with IR set, the same U-mode `rdinstret`
succeeds and returns strictly increasing samples. The gate only applies
below M-mode, so the experiment performs two real privilege drops
(M -> S -> U, one per phase) to observe it. This completes the
`scounteren` family alongside `src/scounteren-cy-gate` (CY) and
`src/scounteren-tm-gate` (TM); it does not re-verify
`src/mcounteren-ir-gate`, which gated S-mode reads via `mcounteren`. The
module shares only `src/boot.S` and `src/uart.c` with the other demos.

- `scir_main.c`: M-mode setup (boot `scounteren` readback, 0x7
  writability probe, gate write of 0, `mcounteren.IR` set so the
  M-level gate does not mask the S-level gate under test,
  `mtvec`/`stvec`/`sscratch` install, `medeleg` bits 2 and 8 so the
  illegal-instruction trap and the U-mode `ecall` are delivered to
  S-mode while all other traps stay in M-mode, a
  whole-address-space PMP NAPOT entry, then `mret` with MPP=01 into
  the S-mode driver). The S-mode driver runs phase A (`sret` with
  SPP=0 into the U-mode payload A with IR clear, expecting exactly
  two S-mode traps: the gated `rdinstret` with `scause = 2` and `sepc`
  exactly at the rdinstret site, then the payload's `ecall` signal),
  then sets `scounteren.IR` itself and runs phase B (`sret` into
  the U-mode payload B, expecting no new trap from either
  `rdinstret` and two strictly increasing samples delivered in the
  interrupted t0/t1 of the closing `ecall`). A 16-instruction nop
  sequence separates the two reads. A 64-bit FNV-1a checksum is fed
  the eighteen verdict-relevant values in a fixed order from every
  privilege level and printed on the completion path. Only
  run-invariant values are fed (causes, counts, check booleans),
  never raw counter samples, so the checksum is identical on every
  passing run.
- `scir_trap.S`: S-mode trap entry (direct mode). Records
  `scause`/`stval`/`sepc` and the interrupted t0/a0/t1/s0, appends
  each trap to an 8-entry history, and calls the C dispatcher
  `scir_handle`, which returns the resume pc: `sepc+4` for the
  gated `rdinstret` (exact: `rdinstret` has no compressed encoding),
  or an S-mode continuation for the U-mode `ecall` signals
  (setting `sstatus.SPP=1` first so `sret` resumes in S-mode). Also
  holds the M-mode park handler (any trap reaching M-mode is
  unexpected) and both U-mode payloads with assembler-resolved
  site labels (`scir_u_rdinstret_a`, `scir_u_ecall_a`,
  `scir_u_rdinstret_b`, `scir_u_ecall_b`); the C `&&label` construct
  is never used for trap-resume addresses.
- `PROOF.md` (this file), `bench-logs/` with the build log, three
  raw QEMU run logs, and `host-cpu.txt`.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 18 checks held. On PASS the machine is
shut down through the virt test-device finisher (QEMU exits 0); on
FAIL the hart parks in a `wfi` loop without touching the finisher.

Build: `make scounteren-ir-gate.elf` with the repo Makefile
(`-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie
-fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`),
logged in `bench-logs/build.log`.
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel scounteren-ir-gate.elf` under `timeout` so a parked-hart FAIL
is observable as exit status 124.

## Configuration under test

- Hart: hart 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the experiment drops to S-mode via
  `mret` with `mstatus.MPP = 01`, then to U-mode via `sret` with
  `sstatus.SPP = 0`.
- QEMU 8.2.2 `virt` machine (from `~/workspace/qemu`),
  `riscv64-unknown-elf-gcc` 13.2.0 (Debian), `-march=rv64imac_zicsr`.
- `medeleg = 0x104` (illegal-instruction trap and U-mode `ecall`
  delegated to S-mode); all other traps stay in M-mode and park.

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`. All three
runs exited 0 via the test-device finisher. The three logs are
byte-identical except for the absolute `rdinstret` sample values
(which are a live counter) and their derived deltas; every
verdict-relevant line is identical across runs.

| step | operation | measured readback |
|---|---|---|
| boot | `csrr scounteren` | 0x0 on all 3 runs |
| probe | `csrw scounteren, 0x7`; readback | 0x7 on all 3 runs |
| gate | `csrw scounteren, 0`; readback | 0x0 on all 3 runs |
| unblock | `csrw mcounteren, 0x4` (IR); readback | 0x4 on all 3 runs |
| setup | `medeleg` readback | 0x104 on all 3 runs |
| phase A | U-mode `rdinstret` at 0x80000250 with `scounteren.IR = 0` | trap, `scause = 0x2`, `stval = 0xc02022f3`, `sepc = 0x80000250` (exactly the rdinstret site), interrupted t0 still the 0xdeadbeefdeadbeef sentinel |
| phase A | U-mode `ecall` at 0x8000025c (SIG_A) | trap, `scause = 0x8`, `sepc = 0x8000025c` (exactly the payload ecall site), a0 = 0x9a9a; trap count 2 |
| phase B | `csrs scounteren, IR` in S-mode; readback | 0x4 on all 3 runs |
| phase B | U-mode `rdinstret` x2 with `scounteren.IR = 1` | no new trap from either read; closing `ecall` (SIG_B) at 0x800002b0 with `scause = 0x8`, a0 = 0x9b9b; samples strictly increasing: 0x3eea0102c7c -> 0x3eea0105fd0 (delta 13140), 0x3eea896b4fb -> 0x3eea896e903 (delta 13320), 0x3eeaf580321 -> 0x3eeaf5833e1 (delta 12480); trap count 3 |

Checks: 18 (scounteren probe readback 0x7, scounteren zero
readback, `mcounteren.IR` set, `medeleg` bits 2 and 8, phase-A
trap count 2, trap 1 `scause` 2, trap 1 `sepc` at the rdinstret
site, trap 1 t0 sentinel intact, trap 2 `scause` 8, trap 2 `sepc`
at the payload ecall site, trap 2 signal SIG_A, `scounteren.IR`
set readback, phase-B trap count 3, trap 3 `scause` 8, trap 3
`sepc` at the payload ecall site, trap 3 signal SIG_B, samples
strictly increasing, sample 0 nonzero). Mismatches: 0.
FNV-1a checksum over the eighteen verdict-relevant values:
0x9756843e0befd207, identical on all 3 runs.

Note: `stval = 0xc02022f3` is QEMU's informational readback of the
faulting `rdinstret t0` (`csrrs t0, instret, x0`) encoding for the
illegal-instruction trap; it is printed as observed and is not
part of the verdict or the checksum.

## Limits of verification

- The +4 `sepc` advance in the S-mode handler is exact only
  because `rdinstret` has no compressed encoding; the module asserts
  the result (`sepc` exactly at the labeled rdinstret site, and the
  resume landing exactly on the labeled `ecall`, whose own trap
  record confirms it) rather than assuming it.
- The gate is exercised for U-mode only; S-mode `rdinstret`
  gating by `scounteren.IR` was not tested here (that is
  `src/mcounteren-ir-gate`'s territory via `mcounteren`).
- QEMU-specific readbacks observed but not asserted: boot
  `scounteren = 0x0` and the `stval` encoding above.
- On QEMU 8.2.2 without `-icount`, the `instret` counter advances
  with host time, not as a deterministic per-retired-instruction
  count. Evidence: the two reads are separated by exactly 17
  retired guest instructions (one `rdinstret` plus 16 nops), yet the
  measured deltas are 13140, 13320, and 12480, varying run to run.
  The 16-nop sequence is retained as designed, but the honest
  phase-B verdict is "two reads, strictly increasing, from a live
  counter", and no claim is made that the nop sequence produced
  the observed delta. (Same basis as the `rdcycle` gate in
  `src/scounteren-cy-gate`, whose back-to-back reads can likewise
  land far apart in host ticks.)
- No restore of the CSRs is performed: the machine halts on the
  completion path.
