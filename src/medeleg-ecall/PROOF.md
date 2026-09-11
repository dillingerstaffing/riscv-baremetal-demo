<!-- PROOF-HEADER
Checks: 17
Mismatches: 0
Checksum: 0x024788976af70a1b
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: medeleg bit-9 S-mode ecall delegation routing (backlog item 163)

## What was built

`src/medeleg-ecall/`: a bare-metal RISC-V program that proves the
`medeleg` bit 9 (supervisor environment call) actually routes a
trap, not just that the bit is writable. In M-mode on the QEMU
`virt` board it sets `medeleg` bit 9, drops to S-mode, and executes
`ecall`; the S-mode handler must observe the trap with `scause =
0x9`, `sepc` at the ecall, and `sstatus.SPP = 1`, while the M-mode
handler must see zero traps. It then clears `medeleg` bit 9,
re-enters S-mode, and executes `ecall` again; this time the M-mode
handler must observe `mcause = 9` with `mepc` at the ecall site and
the S-mode trap count must not advance. Four files, sharing only
`src/boot.S` and `src/uart.c` with the other demos. Exactly one
mechanism is under test: which privilege mode's handler receives
an S-mode ecall as a function of `medeleg` bit 9.

- `med_trap.S`: M-mode trap entry. Bumps the M-mode trap counter
  in `med_regs[0]`, records `mcause`/`mepc` in slots 1-2. For
  `mcause = 9` it records the cause/address into slots 3-4, bumps
  the ecall counter in slot 5, advances `mepc` by 4 (ecall has no
  compressed encoding), and returns with `mret`. For `mcause = 2`
  (the deliberate illegal instruction the S-mode payloads use as
  the phase-complete signal) it bumps the phase-complete counter
  in slot 6, points `mepc` at `med_phase2` or `med_finish`,
  sets `mstatus.MPP` to M-mode, and `mret`s. Any other `mcause`
  parks the hart, which the bench harness observes as a timeout.
- `med_strap.S`: S-mode trap entry. Records `scause`, `stval`,
  the hardware-saved `sepc`, and the `sstatus.SPP` bit, bumps the
  S-mode trap counter, advances `sepc` by 4, and returns with
  `sret`.
- `med_main.c`: installs direct-mode `mtvec`/`stvec` and the
  `mscratch`/`sscratch` save areas, probes `medeleg` bit-9
  writability (write 0x200, require the readback to have bit 9
  set), opens a whole-address-space PMP NAPOT entry (S-mode is
  default-deny without one), then runs the two phases through
  `mret` drops and checks all 17 conditions below. A failed check
  prints `FAIL` and flips the verdict; `RESULT: PASS` is printed
  only when every check held, followed by the FNV-1a checksum over
  the verdict values.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build (manual, same style as the scounteren-trap module; no
Makefile stanza was added so the parallel worker's Makefile edit
cannot conflict):

```
riscv64-unknown-elf-gcc [flags] -c src/boot.S -o src/boot.o
riscv64-unknown-elf-gcc [flags] -c src/uart.c -o src/uart.o
riscv64-unknown-elf-gcc [flags] -c src/medeleg-ecall/med_main.c -o src/medeleg-ecall/med_main.o
riscv64-unknown-elf-gcc [flags] -c src/medeleg-ecall/med_trap.S -o src/medeleg-ecall/med_trap.o
riscv64-unknown-elf-gcc [flags] -c src/medeleg-ecall/med_strap.S -o src/medeleg-ecall/med_strap.o
riscv64-unknown-elf-gcc [flags] -T link.ld -o medeleg-ecall.elf \
    src/boot.o src/uart.o src/medeleg-ecall/med_trap.o \
    src/medeleg-ecall/med_strap.o src/medeleg-ecall/med_main.o
```

with `[flags]` = `-Wall -Wextra -O2 -ffreestanding -nostdlib
-nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr
-mabi=lp64 -mcmodel=medany`.

Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel medeleg-ecall.elf`

Toolchain: xpack riscv-none-elf-gcc 15.2.0 (via
`~/workspace/toolchains/compat-bin`), QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart, QEMU boots the ELF straight into
  M-mode with `-bios none`.
- Boot `medeleg` reads `0x0` on this board; the probe write of
  `0x200` reads back `0x200`, proving bit 9 is writable and that
  the later clear is a real state change, not a silently ignored
  write.
- No interrupts are enabled; `mideleg` is untouched. The only
  traps the run takes are the two `ecall`s under test and the two
  deliberate illegal instructions used to return control to
  M-mode.

## Sequence and controls

1. Record boot `medeleg`; probe bit 9 by writing `0x200` and
   requiring the readback to have bit 9 set.
2. Phase 1 with `medeleg = 0x200`: `mret` with MPP=01 into the
   S-mode payload, which executes `ecall` at a labeled 4-byte
   site. Six checks: exactly one S-mode trap, `scause = 9`,
   `sepc` at the ecall site, `sepc+4` at the resume label,
   `sstatus.SPP = 1`, and M-mode trap count still 0. The payload
   then issues a deliberate `.word 0` illegal instruction;
   `medeleg` leaves cause 2 in M-mode, and the M-mode handler
   jumps to phase 2.
3. Phase 2 in M-mode: require the M-mode trap count to be 1
   (only the phase-complete trap; the delegated ecall never
   reached M-mode), clear `medeleg` and require the readback to
   be exactly 0, then `mret` with MPP=01 into the second S-mode
   payload.
4. Phase 2 in S-mode with `medeleg = 0`: execute `ecall` at a
   labeled site. Four checks: M-mode trap count advanced to 2,
   `mcause = 9`, `mepc` at the ecall site, S-mode trap count
   unchanged from phase 1. The payload again issues the illegal
   instruction; the handler jumps to the verdict.
5. Verdict: M-mode trap count 3, phase-2 ecall count 1,
   phase-complete count 2, S-mode trap count unchanged; print the
   FNV-1a checksum over all verdict values.

## Measured results

Identical on all three runs (byte-identical UART output, same
md5):

| quantity | measured |
|---|---|
| boot `medeleg` | `0x0` |
| probe write `0x200` readback | `0x200` |
| phase 1 `scause` | `0x9` |
| phase 1 `sepc` / `sepc+4` | `0x800003ec` / `0x800003f0` (exactly the labeled ecall/resume sites) |
| phase 1 `stval` | `0x0` |
| phase 1 S-mode traps / `sstatus.SPP` | `1` / `1` |
| phase 1 M-mode traps | `0` |
| M-mode traps after phase 1 | `1` (the phase-complete illegal instruction only) |
| `medeleg` after clear | `0x0` |
| phase 2 `mcause` | `0x9` |
| phase 2 `mepc` | `0x80000694` (exactly the labeled ecall site) |
| phase 2 M-mode traps / S-mode traps | `2` / `1` (unchanged from phase 1) |
| final M-mode traps / phase-2 ecalls / phase-completes / S-mode traps | `3` / `1` / `2` / `1` |
| FNV-1a checksum over verdict values | `0x024788976af70a1b` |
| checks / mismatches / verdict | `17` / `0` / `PASS` |

## Raw QEMU output

### Run 1 (bench-logs/run1.log)

```

========================================
medeleg-ecall: S-mode ecall delegation routing
bit 9 set: S-mode handles, bit 9 clear: M-mode handles
========================================

boot: medeleg=0x0
probe: csrw medeleg, 0x200; readback=0x200
setup complete; dropping to S-mode for phase 1...
in S-mode; phase 1: medeleg bit 9 set
(S-mode ecall must be delegated to S-mode)

phase 1 results:
  ecall site (label 1)  = 0x800003ec
  resume site (label 2) = 0x800003f0
  scause  = 0x9
  stval   = 0x0
  sepc    = 0x800003ec
  sepc+4  = 0x800003f0
  S-mode traps  = 1
  sstatus.SPP   = 1
  M-mode traps  = 0

phase 2: medeleg bit 9 clear
(S-mode ecall must trap to M-mode)

  M-mode traps after phase 1 = 1
  medeleg after clear = 0x0
dropping to S-mode for phase 2...
in S-mode; phase 2 ecall (expect M-mode trap):
phase 2 results:
  ecall site (label 1) = 0x80000694
  M-mode traps  = 2
  mcause (ecall) = 0x9
  mepc (ecall)   = 0x80000694
  S-mode traps  = 1 (phase 1 ended at 1)

final trap counts:
  M-mode traps   = 3
  phase-2 ecalls = 1
  phase-completes= 2
  S-mode traps   = 1

checksum (FNV-1a over verdict values) = 0x024788976af70a1b
checks=17 mismatches=0
RESULT: PASS
```

Runs 2 and 3 (bench-logs/run2.log, bench-logs/run3.log) are
byte-identical to run 1, same md5
(`126f3077196375416ff48eec7ca197d5`), same checksum, same
verdict.

## Limits

- Emulator, not silicon: routing behavior is QEMU 8.2.2's model
  of the `virt` board. The privileged specification requires
  `medeleg` bit 9 to delegate supervisor environment calls, so
  the routed behavior matches the specification, but a physical
  core's exact `medeleg` reset value may differ from the `0x0`
  observed here.
- The two phase-complete illegal instructions (`.word 0`) are
  harness plumbing, not part of the mechanism under test; they
  are the deliberate M-mode door the payloads use to return
  control, and their counts are asserted in the verdict so they
  cannot hide an unexpected trap.
- No interrupts were involved; interrupt delegation (`mideleg`)
  was not exercised.
