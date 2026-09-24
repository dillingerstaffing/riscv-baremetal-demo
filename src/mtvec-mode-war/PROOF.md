<!-- PROOF-HEADER
Checks: 10
Mismatches: 0
Checksum: 0x6e1dc83f46d9431
Environment: QEMU 8.2.2
Verdict: PASS
-->
# Proof: mtvec MODE WARL write/readback probe

## What was built

`src/mtvec-mode-war/`: a bare-metal RISC-V program that measures
the legalization behavior of the mtvec CSR on writes to its MODE
field and on an all-ones write, in M-mode on the QEMU `virt` board.
Three files, sharing only `src/boot.S` and `src/uart.c` with the
other demos. Exactly one mechanism is under test: what a written
mtvec value reads back as (which MODE encodings the implementation
provides, how it handles a reserved MODE encoding, whether BASE
bits stick).

- `mmw_trap.S`: M-mode trap entry recording mcause/mepc/mtval and
  bumping a trap counter in the `mmw_regs` array via mscratch.
  mstatus.MIE stays clear for the whole run, so the handler is
  expected never to fire; it advances mepc past the trapping
  instruction so a surprise trap cannot loop silently.
- `mmw_main.c`: records the boot baselines (mtvec, mstatus),
  verifies mstatus.MIE is clear at boot, installs direct-mode
  mtvec and mscratch, then runs phase 1 (`csrw mtvec, all-ones`,
  publish the readback; `csrw mtvec, all-ones-BASE with MODE=0`,
  publish the readback), phase 2 (write MODE=1 at the handler
  address, publish the readback; write MODE=0 at the handler
  address, publish the readback), and phase 3 (restore mtvec to
  the exact boot value, verify bit-for-bit readback; verify
  mstatus is unchanged bit-for-bit). A failed check prints `FAIL`
  and increments the mismatches counter; `RESULT: PASS` is printed
  only when every check held. The verdict-relevant values feed a
  64-bit FNV-1a digest printed as the last data line, so the
  three bench runs can be compared for byte-identical output. On
  PASS the machine is shut down through the virt test-device
  finisher (QEMU exits 0); on FAIL the hart parks without touching
  the finisher.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make mtvec-mode-war.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mtvec-mode-war.elf` (or `make run-mtvec-mode-war`),
under `timeout` so a parked-hart FAIL is observable as exit status
124.

Toolchain: Ubuntu `riscv64-unknown-elf-gcc` 13.2.0
(`~/workspace/toolchains/ubuntu-rv64/usr/bin`),
`-march=rv64imac_zicsr`. QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: hart 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- mstatus.MIE reads clear at boot and at the end, and no
  interrupt enable is ever set, so no interrupt can be taken; the
  trap count must be 0. This is distinct from the done
  `mtvec-vectored` and `mtvec-mode0-direct` modules, which proved
  trap delivery behavior in each mode, not register write/readback
  legalization.

## Measured results (3 QEMU runs, byte-identical)

Raw logs: `bench-logs/run1.log`, `run2.log`, `run3.log`. All three
runs printed byte-identical output, `checks: 10 mismatches: 0`,
`digest: 0x6e1dc83f46d9431`, `RESULT: PASS`, and QEMU exited 0
via the test-device finisher on every run.

| phase | operation | measured readback |
|---|---|---|
| boot | `csrr mtvec`, `csrr mstatus` | mtvec=0x0, mstatus=0xa00000000 |
| 1: ones | `csrw mtvec, 0xffffffffffffffff` (MODE=3, reserved) | mtvec=0x800001c8 (write fully ignored; previous value kept) |
| 1: ones-base | `csrw mtvec, 0xfffffffffffffffc` (all-ones BASE, MODE=0) | mtvec=0xfffffffffffffffc (every BASE bit sticks) |
| 2: vectored | `csrw mtvec, 0x800001c9` (MODE=1) | mtvec=0x800001c9 (MODE=1 sticks; BASE intact) |
| 2: direct | `csrw mtvec, 0x800001c8` (MODE=0) | mtvec=0x800001c8 (MODE=0 sticks; BASE intact) |
| 3: restore | `csrw mtvec, boot mtvec` | mtvec=0x0 (bit-for-bit == boot), mstatus=0xa00000000 (bit-for-bit == boot) |
| traps | handler installed, mstatus.MIE=0 throughout | trap count = 0 on every run |

Legalization, grounded in the readbacks: MODE=0 (direct) and
MODE=1 (vectored) both stick, so this implementation provides
both modes. A write with a reserved MODE encoding (3, on the
all-ones write; 2 and 3 confirmed separately in a scratch probe)
is dropped entirely: the readback is the previous value, not a
legalized variant. The all-ones BASE with MODE=0 reads back
0xfffffffffffffffc, so every BASE bit is writable and the drop
was about the MODE encoding, not about BASE narrowing. mtvec was
restored to the exact boot value (0x0) at the end, and mstatus
was unchanged bit-for-bit.

## What was verified, and what was not

Verified: on QEMU 8.2.2, the boot mtvec reads 0x0; an all-ones
write (reserved MODE=3) is ignored with the previous value
preserved; 0xfffffffffffffffc reads back exactly; MODE=1 and
MODE=0 writes at a 4-byte-aligned handler address each read back
exactly as written; the boot mtvec value restores bit-for-bit;
mstatus is unchanged bit-for-bit; 0 traps; the three runs were
byte-identical (digest 0x6e1dc83f46d9431).

Not verified: behavior on real silicon. These numbers come from
the QEMU 8.2.2 CSR model, not from hardware. Also not verified:
trap delivery in each mode (deliberately out of scope: the done
`mtvec-vectored` and `mtvec-mode0-direct` modules cover delivery),
and any S-mode view of the register.

## Limits

- The module assumes mstatus.MIE is clear at boot (checked, and the
  run fails loudly if it is not); with MIE clear, no mtvec value
  can cause a trap to be taken.
- The reserved-MODE drop behavior is this emulator's WARL choice;
  the spec permits an implementation to legalize a reserved
  encoding differently. The module asserts the measured behavior
  only under QEMU 8.2.2.
- The boot mtvec value 0x0 and mstatus 0xa00000000 are this
  emulator's choice; the module restores the recorded boot value
  rather than assuming zero.
