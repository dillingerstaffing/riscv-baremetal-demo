<!-- PROOF-HEADER
Checks: 16
Mismatches: 0
Checksum: 0x68cbef47360252de
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mcause bit 63, exception vs interrupt

## What was built

`src/mcause-interrupt-bit/`: a bare-metal RISC-V program (M-mode
throughout, no OpenSBI) that tests the rule that `mcause` bit 63 is
set for an asynchronous interrupt and clear for a synchronous
exception. Two deliberate traps:

- Phase 1 (exception): an M-mode `ecall` at a labeled site. The
  handler records `mcause` (expect `0xb`), publishes
  `(mcause >> 63)` (expect 0), checks the recorded `mepc` against the
  ecall instruction's link-time address, and resumes past the 4-byte
  `ecall`.
- Phase 2 (interrupt): the CLINT machine timer interrupt, armed via
  `mtimecmp` 50000 mtime ticks (5 ms) ahead with a bounded re-arm
  retry. The handler records `mcause` (expect
  `0x8000000000000007`), publishes `(mcause >> 63)` (expect 1),
  disarms `mtimecmp` to all-ones, and resumes into the wait loop. A
  quiet window after the disarm proves no re-delivery.

Every expectation is an in-program check (16 checks, 0 mismatches).
A control window with no source armed must produce zero traps, and
zero unexpected traps are allowed across the whole run. The program
publishes an FNV-1a checksum over the recorded values (deterministic
fields only: checks, mismatches, both mcause values, both bit-63
values, both trap counts, unexpected count).

Four files, sharing only `src/boot.S` and `src/uart.c` with the other
demos:

- `mib_main.c`: the test sequence, UART reporting, check/mismatch
  counters, the FNV-1a checksum, and the PASS/FAIL verdict.
- `mib_trap.S`: the M-mode trap entry (direct-mode `mtvec`,
  `mscratch` scratch area).
- `README.md`: module summary.
- `PROOF.md` (this file), `bench-logs/` with the build log, three
  raw QEMU run logs, and the host CPU info.

Build: `make mcause-interrupt-bit.elf` (riscv64-unknown-elf-gcc
13.2.0; added to `all` in the Makefile; `run-mcause-interrupt-bit`
target added too).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mcause-interrupt-bit.elf` (QEMU 8.2.2; the program parks in
`wfi` after printing `done`, so the logs below were taken under
`timeout 60`, exit status 124 from the timeout kill, not the guest).

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/boot.S -o src/boot.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/uart.c -o src/uart.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mcause-interrupt-bit/mib_trap.S -o src/mcause-interrupt-bit/mib_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/mcause-interrupt-bit/mib_main.c -o src/mcause-interrupt-bit/mib_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o mcause-interrupt-bit.elf src/boot.o src/uart.o src/mcause-interrupt-bit/mib_trap.o src/mcause-interrupt-bit/mib_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: mcause-interrupt-bit.elf has a LOAD segment with RWX permissions
```

(The RWX note is the linker's standard remark for these bare-metal
builds, present on the sibling modules too.)

## Run output (all three runs byte-identical except the timeout-kill line)

```
mcause-interrupt-bit: mcause bit 63, exception vs interrupt
trap: mtvec=0x800001c8
control: traps-before-ecall=0 (expect 0)
ecall: count=1 mcause=0xb bit63=0 mepc=0x800003e0 (expect mcause=0xb bit63=0)
timer: armed=1 (mtimecmp set 50000 ticks ahead, up to 10 arm attempts)
timer: count=1 mcause=0x8000000000000007 bit63=1 (expect mcause=0x8000000000000007 bit63=1)
timer: mtimecmp-after-handler=0xffffffffffffffff (expect 0xffffffffffffffff)
timer: count-after-quiet=1 (expect 1)
unexpected: count=0 (expect 0)
checks=16 mismatches=0 checksum=0x68cbef47360252de
RESULT: PASS
done
```

Run 1, run 2, run 3: identical to the byte on every line above
(including `armed=1` on all three runs); the final
`qemu-system-riscv64: terminating on signal 15 from pid ...` line
differs only in the host pid, which is not verdict-relevant.

## What was verified

- `mtvec` took the handler address in direct mode (2 checks).
- Zero traps in the control window with no source armed (1 check):
  the two traps below came from their own triggers.
- M-mode ecall: trapped exactly once, `mcause = 0xb`, bit 63 = 0,
  recorded `mepc` equals the ecall instruction address (5 checks).
- Machine timer interrupt: armed in the future, delivered exactly
  once, `mcause = 0x8000000000000007`, bit 63 = 1 (4 checks).
- Handler disarmed `mtimecmp` (reads all-ones) and no re-delivery
  arrived in the quiet window (2 checks).
- Zero unexpected traps across the whole run (1 check, plus the
  mcause value printed if any fired).
- 16 checks, 0 mismatches, FNV-1a checksum `0x68cbef47360252de`
  over the recorded values, identical on all three runs.

## Honest limits

- This is emulator behavior (QEMU 8.2.2, virt machine), not silicon:
  the bit-63 rule is verified against QEMU's trap delivery.
- Only two trap classes were exercised: an M-mode ecall and the
  CLINT machine timer interrupt. Supervisor/external interrupts,
  other exception codes, and delegated traps are out of scope.
- The 2026-09-10 attempt of this item used OpenSBI software
  interrupts and was blocked on pending-bit ambiguity there; this
  retry deliberately avoids that setup and claims nothing about it.
- The `timer: armed=1` line depends on host scheduling inside the
  5 ms arm window; a descheduled host could in principle force a
  re-arm (the retry loop) or, after 10 failed arms, a FAIL. All
  three runs armed on the first attempt. The checksum covers only
  the deterministic recorded values.
