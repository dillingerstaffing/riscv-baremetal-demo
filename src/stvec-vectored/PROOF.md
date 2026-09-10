<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF.md: stvec vectored offsets (backlog item 133)

## What was built

A bare-metal RV64 module that installs `stvec` in vectored mode
(MODE=1) over a 16-entry table of single 32-bit `jal` stubs in S-mode
on QEMU 8.2.2 (`virt`), delegates the supervisor timer interrupt
(`mideleg` bit 5) and the supervisor external interrupt (`mideleg`
bit 9) to S-mode, raises both, and checks that each trap lands at
`BASE + 4*code` with the correct `scause`: entry 5 for the timer
interrupt (code 5 -> BASE + 20), entry 9 for the external interrupt
(code 9 -> BASE + 36). Each stub records the link-time address of its
own table entry via an assembler-resolved `la`, so the landing
address is measured, not assumed.

Files: `stvv_trap.S`, `stvv_main.c`. Raw QEMU logs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(byte-for-byte identical across three boots). Build log:
`bench-logs/build.log`.

## Build log (genuine)

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/boot.S -o src/stvec-vectored/stvv_boot.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/uart.c -o src/stvec-vectored/stvv_uart.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/stvec-vectored/stvv_trap.S -o src/stvec-vectored/stvv_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/stvec-vectored/stvv_main.c -o src/stvec-vectored/stvv_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o stvec-vectored.elf src/stvec-vectored/stvv_boot.o src/stvec-vectored/stvv_uart.o src/stvec-vectored/stvv_trap.o src/stvec-vectored/stvv_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: stvec-vectored.elf has a LOAD segment with RWX permissions
```

The RWX-segment warning is the standard flat-image linker note and
appears for every module in this repo. One compile issue was found
and fixed during bring-up: `csrci sie, 32` / `csrci sie, 512` were
rejected because `csr` immediate operands are 5 bits; the clears use
`csrc sie, <reg>` instead.

## Measured results (QEMU 8.2.2, `virt`, all three runs)

From `run1.log` (identical in run2/run3):

```
stvec-vectored: S-mode vectored stvec offset test
sstc: stce=1
mideleg: write=0x0 readback=0x1444
mideleg: write=0x220 readback=0x1664
medeleg: write=0x0 readback=0x0
stvec: written=0x80000201 readback=0x80000201
plic: priority[10]=1 enable10=1 thresh=0
smode: stvec=0x80000201 base=0x80000200 mode=1
table: base=0x80000200 vec5=0x80000214 (expect 0x80000214) vec9=0x80000224 (expect 0x80000224)
timer: armed (delta=2000000)
timer: ev_count=1 scause=0x8000000000000005 landing=0x80000214 idx=5 (expect 0x80000214) stimecmp=0xffffffffffffffff
ext: asserted, pending observed
ext: ev_count=2 scause=0x8000000000000009 landing=0x80000224 idx=9 (expect 0x80000224) claim=10
RESULT: PASS
```

Decoded:

- Sstc present (`menvcfg.STCE` stuck at 1), so the timer phase arms
  `stimecmp` directly.
- `mideleg` zero-write readback `0x1444`: this hart ORs the
  hypervisor interrupt bits (2, 6, 10, 12) back in after every write
  (same forced set measured by `src/mideleg-route/`). Writing `0x220`
  reads back `0x1664`, exactly the forced set plus bits 5 and 9:
  the supervisor timer and supervisor external interrupts are
  delegated and nothing else among the software-writable bits is.
- `medeleg` takes and reads back 0: no exceptions delegated.
- `stvec` written and read back as `0x80000201`: BASE `0x80000200`,
  MODE 1 (vectored), verified in both M-mode and S-mode.
- Table self-check: entry 5 sits at `0x80000214` = BASE + 20 and
  entry 9 at `0x80000224` = BASE + 36; objdump confirms all 16
  entries are one 32-bit `jal`, 4 bytes apart.
- Timer interrupt: exactly one trap, `scause = 0x8000000000000005`,
  landing address `0x80000214` (BASE + 20), entry index 5, recorded
  by the stub that ran. The handler disarmed the source:
  `stimecmp` reads back `0xffffffffffffffff`.
- External interrupt: exactly one trap, `scause =
  0x8000000000000009`, landing address `0x80000224` (BASE + 36),
  entry index 9, PLIC claim returned source 10 (UART0).
- Totals: 2 traps, 0 unexpected, 0 M-mode traps during setup.

`cmp` confirms `run1.log`, `run2.log`, and `run3.log` are
byte-for-byte identical; the QEMU process exited 0 on all three
runs (the module shuts the machine down via the virt test-device
finisher only after printing `RESULT: PASS`).

## Limits

- Emulator, not silicon: all behavior above was measured on QEMU
  8.2.2 (`virt` machine). Real hardware vectors the same way per
  the privileged spec, but the addresses, the `mideleg` forced set,
  and the Sstc presence are properties of this emulator build.
- CSR encodings (`scause` values, `stvec` MODE=1, `mideleg` bits 5
  and 9) are architectural; the linked addresses (`0x80000200`,
  `0x80000214`, `0x80000224`) are specific to this image and run.
- The timer phase depends on Sstc (`menvcfg.STCE`); the module
  refuses to claim a result on a hart without it.
- Toolchain: `riscv64-unknown-elf-gcc` 13.2.0, `-O2`.
