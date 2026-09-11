<!-- PROOF-HEADER
Checks: 17
Mismatches: 0
Checksum: 0xddf55eeede88e646
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: menvcfg.STCE advertisement agrees with real Sstc presence

## What was built

`src/menvcfg-stce/`: a bare-metal RISC-V program that checks the
machine-mode Sstc advertisement against reality on the QEMU `virt`
board. `menvcfg` bit 63 (STCE) is the architectural advertisement:
when it is 1, S-mode may access the `stimecmp` CSR; when it is 0,
S-mode accesses raise an illegal-instruction exception. The module
reads `menvcfg`, probes the bit's WARL behavior, then drops to
S-mode and performs a real `stimecmp` access. The advertisement is
honest exactly when (STCE == 1) matches (the access succeeds). When
the access succeeds, the module goes further and arms a supervisor
timer interrupt from S-mode, requiring exactly one delegated trap
with `scause = 0x8000000000000005` and a quiet window with zero
re-delivery traps, which is the functional proof that Sstc is
really there. A disagreement in either direction fails the run and
the log says which direction. The module shares only `src/boot.S`
and `src/uart.c` with the other demos.

- `menv_main.c`: M-mode boot (boot `menvcfg` readback, STCE clear
  probe, all-ones WARL probe, boot-value restore, STCE set probe,
  `mideleg` bits 2+5 so the probe fault and the timer interrupt are
  delegated to S-mode, `mcounteren = 0x7`, a whole-address-space
  PMP NAPOT entry, `mtvec`/`stvec`/`sscratch` install, then `mret`
  with MPP=01 into the S-mode payload). The S-mode payload runs
  phase 1 (labeled `stimecmp` read; the delegated handler records
  `scause`/`sepc`, advances `sepc` by 4 for the synchronous
  exception only, and resumes) and, only if the access worked,
  phase 2 (arm `stimecmp` = `mtime` + 10000 from S-mode, one trap
  expected, in-handler disarm to all-ones, then a 100000-`rdcycle`
  quiet window with SIE on). A 64-bit FNV-1a checksum is fed the
  fourteen verdict-relevant values in a fixed order from both
  privilege levels and printed on the completion path. Absolute
  addresses and timing samples are deliberately excluded from the
  checksum.
- `menv_trap.S`: S-mode trap entry (direct mode). Saves all
  general-purpose registers, records `scause` and the original
  `sepc`, advances `sepc` by 4 for exceptions (the probe CSR access
  is a 4-byte instruction emitted under `.option norvc`; interrupts
  keep `sepc` unchanged), then calls the C handler on a dedicated
  trap stack. The C handler disarms `stimecmp` inside the timer
  trap and counts any further trap as bad.
- `menv_mtrap.S` (inside `menv_trap.S`): M-mode trap entry. A
  correct run takes no M-mode trap at all, so this handler records
  `mcause`/`mepc`, prints them, and parks the hart.
- `build.sh`, `PROOF.md` (this file), `bench-logs/` with the build
  log, three raw QEMU run logs, and the host CPU identity.

A failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
is printed only when all 17 checks held. On PASS the machine is
shut down through the virt test-device finisher (QEMU exits 0); on
FAIL the hart parks in a `wfi` loop without touching the finisher.

Build: `build.sh` runs direct `riscv64-unknown-elf-gcc` invocations
matching the repo Makefile pattern (`-Wall -Wextra -O2
-ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic
-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`), logged in
`bench-logs/build.log`. Link order is `boot.o` first: QEMU's
`-kernel` loader starts execution at the load address
(0x80000000), so `_start` must sit there; a link with the trap
object first places `_start` at a nonzero offset and the board
hangs silently (verified: exit 124, zero UART bytes). Every other
module's Makefile section already links `src/boot.S` first for the
same reason.
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel menvcfg-stce.elf` under `timeout`, so a parked-hart FAIL is
observable as exit status 124. QEMU is the 8.2.2 build from
`~/workspace/qemu` (system `qemu-system-riscv64` on this VM fails
with a missing `libfdt.so.1`).

## Configuration under test

- Hart: hart 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the experiment drops to S-mode via
  `mret` with `mstatus.MPP = 01`.
- QEMU 8.2.2 `virt` machine, `riscv64-unknown-elf-gcc` 15.2.0
  (xPack), `-march=rv64imac_zicsr`.
- Host CPU: AMD EPYC 9D64 88-Core Processor
  (`bench-logs/host-cpu.txt`).
- `mideleg` written as `0x24` (bits 2 and 5); readback `0x1464`
  (QEMU sets additional bits 6, 8, 10, 12; observed, not asserted
  beyond the mask).

## Measured results (3 QEMU runs)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs exited 0 via the test-device finisher. The three logs are
byte-identical except for the absolute `stimecmp` arm value and the
`rdcycle` quiet-window delta (both are live timing values); every
verdict-relevant line is identical across runs.

| step | operation | measured readback |
|---|---|---|
| boot | `csrr menvcfg` | 0x2000000000000000, STCE(bit63)=0, on all 3 runs |
| clear probe | `csrc menvcfg, STCE`; readback | 0x2000000000000000 (equals boot with bit 63 clear) on all 3 runs |
| WARL probe | `csrw menvcfg, all-ones`; readback | 0xa0000000000000f1 on all 3 runs: STCE (bit 63) sticks, so the bit is implemented; bits 61, 7, 6, 5:4, 0 also read back set, the rest legalize to 0 |
| restore | `csrw menvcfg, boot value`; readback | 0x2000000000000000 on all 3 runs |
| set probe | `csrs menvcfg, STCE`; readback | 0xa000000000000000, STCE sticks: yes (Sstc advertised) on all 3 runs |
| setup | `mideleg` readback | 0x1464 on all 3 runs (bits 2 and 5 set as written) |
| phase 1 | S-mode `csrr stimecmp` at 0x800003c2 with STCE=1 | no trap on any run (0 S-mode traps); access worked |
| advertisement | STCE advertised = 1, S-mode access worked = 1 | MATCHES reality on all 3 runs |
| phase 2 | arm `stimecmp` = `mtime`+10000 from S-mode | exactly 1 trap, `scause = 0x8000000000000005`, `sepc` = 0x80000210 inside the arming spin loop [0x80000210, 0x80000218), `sip` STIP clear after the in-handler disarm write, `stimecmp` reads all-ones after disarm |
| quiet window | 100000 `rdcycle` reads, SIE on, `stimecmp`=all-ones | 0 traps on all 3 runs; `stimecmp` still all-ones after; S-mode `stimecmp` writes = 2 (arm + in-handler disarm) |

Checks: 17 (STCE clear readback, STCE set on all-ones write,
restore readback, `mideleg` bits 2+5, `stvec` direct mode, phase-1
trap count 0, advertisement match, post-arm trap count 1, `scause`
is the supervisor timer interrupt, `sepc` inside the spin loop,
STIP clear after disarm, comparator all-ones after disarm, quiet
window ran to completion, 0 quiet-window traps, trap count still
1, comparator still all-ones, S-mode write count 2). Mismatches: 0.
FNV-1a checksum over the fourteen verdict-relevant values:
0xddf55eeede88e646, identical on all 3 runs.

Note: the phase-1 probe readback (`stimecmp` = 0x0 at S-mode entry)
is QEMU's leftover comparator value, printed as observed and
excluded from the verdict and the checksum; a stale 0 below the
current `mtime` means the interrupt was already pending at entry,
but `sie`/SIE were still clear and the arm write clears the stale
pending bit, so the single-trap result is unaffected.

## Limits of verification

- The +4 `sepc` advance in the S-mode entry applies to exceptions
  only; it is exact for the probe because the `stimecmp` CSR access
  is emitted as a 4-byte instruction under `.option norvc`, and the
  module asserts the resumed address equals the labeled resume
  address on the fault path (not exercised on QEMU 8.2.2, where the
  access succeeds).
- The advertisement is checked in the STCE=1 direction only on
  this board; the mismatch path (STCE=0 with working access, or
  STCE=1 with faulting access) is coded and would fail the
  advertisement check, but QEMU 8.2.2 advertises honestly.
- QEMU-specific readbacks observed but not asserted: boot
  `menvcfg` = 0x2000000000000000 (bit 61 set; no meaning assigned),
  the all-ones legalized value 0xa0000000000000f1, and `mideleg`
  readback 0x1464 after writing 0x24.
- The quiet window is 100000 `rdcycle` reads, shorter than the
  1000000 used by `src/stimecmp-one-shot`; the in-handler disarm
  plus zero re-delivery traps over the window is the property under
  test, and the window length is published, not hidden.
- Emulator behavior, not silicon: every number above was measured
  under QEMU 8.2.2 `virt`, not on a physical hart.
