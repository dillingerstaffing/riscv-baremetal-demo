# Proof: ecall ABI round-trip, S-mode payload to M-mode handler

## What was built

`src/ecall/`: a bare-metal RISC-V program that drops from M-mode to
S-mode, issues environment calls, and verifies the M-mode trap handler
preserves the caller-saved registers across the privilege boundary.
Three files, 443 lines total, sharing only `src/boot.S` and `src/uart.c`
with the other demos.

- `ecall_main.c`: UART bring-up, PMP programming for the S-mode drop,
  the privilege transition (sepc + sstatus.SPP + sret, the same pattern
  the smode module uses), the S-mode payload, the M-mode ecall
  dispatcher, and the M-mode report that cross-checks both sides.
- `ecall_trap.S`: M-mode trap entry. Saves every register x1-x31 into
  `ecall_regs` (interrupted t0 via mscratch, save-area base via s0
  across the C call), calls the C dispatcher, restores every register,
  and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make ecall.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel ecall.elf`
(or `make run-ecall`).

## Mechanism under test

One ecall service (number 1 in a7): the handler returns
a0 = a0^a1^a2^a3^a4^a5 and must leave a1-a7 and t0-t6 bit-identical.
A second service (0xE11) ends the test and resumes M-mode for the
report. The payload is a C function running in S-mode; each argument
register is pinned with a `register ... __asm__("a0")` variable so the
compiled `ecall` has the exact register layout (confirmed by reading
the disassembly: all of a0-a7, t0-t6 are loaded immediately before the
`ecall` at 0x8000031c and read back immediately after).

Ground truth the report checks per call: mcause must read 9
(environment call from S-mode), and the instruction word at mepc must
read 0x00000073 (the ecall encoding). The handler-side log (what the
handler received) is compared word for word against the payload-side
constants (what was loaded), and the returned a0 is compared against
an independently computed XOR.

## Configuration under test

- Hart: mhartid = 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`.
- PMP entry 0: pmpaddr0 = all-ones, pmpcfg0 = 0x1f (NAPOT, R/W/X,
  unlocked), covering the whole address space. This is required, not
  optional: with no PMP entry programmed, S-mode has no access to any
  address (M-mode keeps full access, lower modes default-deny), and the
  first S-mode instruction fetch raises an instruction access fault.
  That failure was observed directly while building this module
  (mcause=1, mepc at the S-mode entry) before the PMP entry was added.
- satp = 0 (bare mode): no address translation is involved in this
  test. The payload stack is the same physical stack in both modes.

## Argument sets (3)

| set | a0-a5 | a6 | t0-t6 pattern |
|---|---|---|---|
| 0 | 1, 2, 3, 4, 5, 6 | 0xA6A6A6A6A6A6A6A6 | 0x1111.. to 0x7777.. |
| 1 | 0, all-ones, 0xAA.., 0x55.., 0x8000.., 1 | 0x6A6A6A6A6A6A6A6A | 0x8888.. to 0xEEEE.. |
| 2 | 0x0123.., 0xFEDC.., 0xAA.., 0x55.., 0xDEADBEEFCAFEBABE, 0x0F0F.. | 0x1234567890ABCDEF | mixed edge patterns |

Set 1 carries the edge values: zero, all-ones 0xFFFFFFFFFFFFFFFF,
and the alternating bit patterns 0xAAAAAAAAAAAAAAAA /
0x5555555555555555.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

Program output is byte-identical across all three runs (sha256
422c3a27a54494f17ed6607e2d6a060402e00d23f5abccd38f727075611c
after removing the QEMU timeout notice on stderr). Every run prints
`RESULT: PASS`. Full output of run 1:

```
ecall ABI round-trip: S-mode payload, M-mode handler
dropping to S-mode for the payload...

--- ecall ABI round-trip report (M-mode) ---
call 0: mcause=0x9 mepc=0x8000031c insn=0x73 svc(a7)=0x1
  handler received a0-a5: 0x1 0x2 0x3 0x4 0x5 0x6
  handler returned a0=0x7 resume=0x80000320
call 1: mcause=0x9 mepc=0x8000031c insn=0x73 svc(a7)=0x1
  handler received a0-a5: 0x0 0xffffffffffffffff 0xaaaaaaaaaaaaaaaa 0x5555555555555555 0x8000000000000000 0x1
  handler returned a0=0x8000000000000001 resume=0x80000320
call 2: mcause=0x9 mepc=0x8000031c insn=0x73 svc(a7)=0x1
  handler received a0-a5: 0x123456789abcdef 0xfedcba9876543210 0xaaaaaaaaaaaaaaaa 0x5555555555555555 0xdeadbeefcafebabe 0xf0f0f0f0f0f0f0f
  handler returned a0=0xd1a2b1e0c5f1b5b1 resume=0x80000320
payload view (S-mode, after each ecall):
  set 0: a0 after=0x7 expected=0x7 a1-a5,a6,a7,t0-t6 intact: yes
    after a1-a7: 0x2 0x3 0x4 0x5 0x6 0xa6a6a6a6a6a6a6a6 0x1
    after t0-t6: 0x1111111111111111 0x2222222222222222 0x3333333333333333 0x4444444444444444 0x5555555555555555 0x6666666666666666 0x7777777777777777
  set 1: a0 after=0x8000000000000001 expected=0x8000000000000001 a1-a5,a6,a7,t0-t6 intact: yes
    after a1-a7: 0xffffffffffffffff 0xaaaaaaaaaaaaaaaa 0x5555555555555555 0x8000000000000000 0x1 0x6a6a6a6a6a6a6a6a 0x1
    after t0-t6: 0x8888888888888888 0x9999999999999999 0xaaaaaaaaaaaaaaaa 0xbbbbbbbbbbbbbbbb 0xcccccccccccccccc 0xdddddddddddddddd 0xeeeeeeeeeeeeeeee
  set 2: a0 after=0xd1a2b1e0c5f1b5b1 expected=0xd1a2b1e0c5f1b5b1 a1-a5,a6,a7,t0-t6 intact: yes
    after a1-a7: 0xfedcba9876543210 0xaaaaaaaaaaaaaaaa 0x5555555555555555 0xdeadbeefcafebabe 0xf0f0f0f0f0f0f0f 0x1234567890abcdef 0x1
    after t0-t6: 0xffffffffffffffff 0x0 0xaaaaaaaaaaaaaaaa 0x5555555555555555 0xf0f0f0f0f0f0f0f 0xf0f0f0f0f0f0f0f0 0x8000000000000000
RESULT: PASS
```

What was verified, per call:

- mcause read 9 from the CSR (S-mode environment call), mepc
  pointed at the payload's ecall, and the word at mepc read
  0x00000073 (the ecall encoding).
- The handler's received a0-a5 matched the payload's loaded constants
  exactly, for all three sets, as did a6, a7, and t0-t6 (checked in
  code against the set tables; any mismatch fails the run).
- The handler's returned a0 equaled the XOR of a0-a5: 0x7,
  0x8000000000000001, 0xd1a2b1e0c5f1b5b1. These three values were
  recomputed independently with Python from the set tables and match.
- After each ecall, the payload read back a1-a7 and t0-t6 unchanged
  (all 14 registers, all three sets).

Two defects were found and fixed during development, both caught by
the verification itself: the trap entry's return path reused t1 as
scratch for the resume pc without restoring the interrupted t1
(caught because the payload read back t1 = resume pc instead of its
constant), and the initial S-mode drop faulted until the PMP entry
above was programmed (caught as mcause=1 at the S-mode entry).

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/ecall/ecall_trap.S -o src/ecall/ecall_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/ecall/ecall_main.c -o src/ecall/ecall_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o ecall.elf src/boot.o src/uart.o src/ecall/ecall_trap.o src/ecall/ecall_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: ecall.elf has a LOAD segment with RWX permissions
```

Zero compiler warnings. (The RWX linker warning comes from the
shared link.ld and appears for every module in this repo.)

Toolchain: riscv64-unknown-elf-gcc 13.2.0, QEMU 8.2.2
(`qemu-system-riscv64 --version` reports 8.2.2). The S-mode resume
address uses a plain `la t0, s_payload` symbol reference, not a C
labels-as-values address, because this gcc miscompiles `&&label` at
-O2.

## Limits of verification (stated honestly)

- Emulator, not silicon: all behavior observed is QEMU 8.2.2's
  implementation of the privileged spec (virt machine). Cycle counts
  are not claimed; no timing is measured.
- Single hart (mhartid 0); no SMP interaction tested.
- satp = 0, so no virtual memory is involved; the S-mode and M-mode
  views of memory are the same physical addresses.
- Only the synchronous ecall path is tested. Interrupts are never
  enabled, so nested/interrupted traps are not covered.
- The PMP entry used here is deliberately wide open (whole address
  space, R/W/X); PMP denial behavior is covered by the separate pmp
  module, not this one.
