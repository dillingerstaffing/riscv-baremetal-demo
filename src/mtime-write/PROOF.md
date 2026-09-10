<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: CLINT mtime write/readback/advance measurement (backlog item 135)

## What was built

`src/mtime-write/`: a bare-metal RISC-V program that writes a known
constant to the memory-mapped CLINT `mtime` register, reads it back
immediately, and publishes the write/readback/delta triple, then
verifies the counter advances strictly from the written base. One
file, sharing only `src/boot.S` and `src/uart.c` with the other
demos. Exactly one mechanism is under test: whether a store to
`mtime` takes effect on this machine and whether the counter keeps
advancing afterward.

- `mtw_main.c`: reads `mtime` twice (baseline, the second must be
  larger), writes `0x100000000` as two 32-bit stores (low word, then
  high word), reads back immediately with a stable-pair 32-bit read
  and prints written/readback/delta, then takes four further
  readbacks that must each be strictly larger than the previous
  sample and none below the written base. The write verdict is not
  assumed: the unsigned delta `readback - written` is compared
  against a bound of 100000 mtime ticks, so an ignored write
  (readback below the written value, or far above it) prints the
  actual values and fails the check instead of passing silently.
  The 32-bit access form is used throughout because 64-bit
  accesses to CLINT registers fault on this emulator (observed with
  the `msip` register in `src/msip/`); this module never issues a
  64-bit CLINT access. The written value 2^32 ticks (about 429
  seconds of virtual time at the 10 MHz timebase) is unreachable by
  natural advancement in the few seconds of wall time the machine
  has been up, so readback >= written genuinely discriminates a
  stuck write from an ignored one. On PASS it shuts the machine
  down via the virt test-device finisher so the QEMU process exit
  code (0) reflects the verdict; on FAIL it parks the hart and the
  bench harness observes the timeout exit status (124) plus the
  RESULT line.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build (from the repo root; not wired into the root Makefile because
this commit adds only new files):

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib \
  -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr \
  -mabi=lp64 -mcmodel=medany -c src/mtime-write/mtw_main.c \
  -o src/mtime-write/mtw_main.o
```

linked with `src/boot.o`, `src/uart.o`, and `link.ld` into
`mtime-write.elf`.

Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mtime-write.elf`.

Toolchain: riscv64-unknown-elf-gcc 13.2.0
(`/usr/bin/riscv64-unknown-elf-gcc`, Debian), QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- The program is the only code running; no S-mode, no interrupts
  enabled, no PLIC involvement. The only register under test is the
  memory-mapped `mtime` at 0x0200bff8 on the `virt` CLINT, accessed
  from M-mode with plain 32-bit loads and stores.

## Sequence and controls

1. Calibrate `rdcycle` against `mtime`: spin for 1M mtime ticks and
   print the `rdcycle` delta and the ratio, so the timing numbers are
   interpretable as host time (same construction as `src/msip/`).
2. Read `mtime` twice; the second read must be strictly larger.
   This confirms the counter is live before the write (on QEMU
   without icount, `mtime` is driven by the host wall clock at
   10 MHz, so the counter ticks with wall time between any two
   reads).
3. Write `0x100000000` to `mtime`: 32-bit store of the low word at
   0x0200bff8, then 32-bit store of the high word at 0x0200bffc.
4. Read back immediately with the stable-pair sequence (high, low,
   high; keep the pair only when the two high reads agree) and
   print the written value, the readback, and the unsigned delta
   `readback - written`. The readback must be >= the written value
   and within 100000 ticks of it; a smaller readback (the write did
   not stick) wraps the unsigned delta to a huge value and fails the
   bound check, with the actual numbers printed.
5. Take four further readbacks. Each must be strictly larger than
   the previous sample, and none may fall below the written base.

## Measured results

The write is honored on QEMU 8.2.2: every run read back the written
value plus a few hundred mtime ticks (the time the emulator needed
between the stores and the loads), and the counter advanced strictly
from there. Write/readback/delta triples:

| quantity | run 1 | run 2 | run 3 |
|---|---|---|--- |
| `rdcycle` per mtime tick (1M-tick calibration) | 149 | 149 | 149 |
| baseline `mtime0` / `mtime1` | `0xf9de9` / `0xfa089` | `0xfea87` / `0xfecd6` | `0x1005e7` / `0x100843` |
| baseline delta | 672 | 591 | 604 |
| written | `0x100000000` | `0x100000000` | `0x100000000` |
| readback | `0x1000002b2` | `0x10000021d` | `0x1000001f9` |
| write/readback delta (ticks) | 690 | 541 | 505 |
| write took effect | yes | yes | yes |
| advance samples | `0x1000023cc 0x100002960 0x100002bba 0x100002e0b` | `0x100008693 0x100008c68 0x100008ecf 0x10000912a` | `0x10000236a 0x100006893 0x10000a7e9 0x10000e65a` |
| advance strictly increasing | yes | yes | yes |
| advance all >= written base | yes | yes | yes |
| verdict | PASS | PASS | PASS |

The three run logs are NOT byte-identical: baseline values and
deltas vary with the host clock, as expected for a time-driven
counter. Everything structural is identical across runs: the banner,
the written constant, the "yes" answers, the verdicts. The
write/readback delta sits at 505-690 mtime ticks on all three runs
(50-69 microseconds of virtual time), consistent with the few
instructions between the stores and the immediate read. QEMU exited
with status 0 on all three runs, the exit code that only the
PASS path produces.

## Raw QEMU output

### Run 1 (bench-logs/run1.log)

```
mtime-write: CLINT mtime write/readback/advance measurement
clock: 1000000 mtime ticks -> rdcycle delta 149961630 (ratio 149 rdcycle units per tick)
baseline: mtime0=0xf9de9 mtime1=0xfa089 delta=672
write/readback: written=0x100000000 readback=0x1000002b2 delta=690
write took effect (readback >= written, delta <= 100000): yes
advance: 0x1000023cc 0x100002960 0x100002bba 0x100002e0b
advance samples strictly increasing: yes
advance samples all >= written base: yes
RESULT: PASS
```

### Run 2 (bench-logs/run2.log)

```
mtime-write: CLINT mtime write/readback/advance measurement
clock: 1000000 mtime ticks -> rdcycle delta 153045375 (ratio 149 rdcycle units per tick)
baseline: mtime0=0xfea87 mtime1=0xfecd6 delta=591
write/readback: written=0x100000000 readback=0x10000021d delta=541
write took effect (readback >= written, delta <= 100000): yes
advance: 0x100008693 0x100008c68 0x100008ecf 0x10000912a
advance samples strictly increasing: yes
advance samples all >= written base: yes
RESULT: PASS
```

### Run 3 (bench-logs/run3.log)

```
mtime-write: CLINT mtime write/readback/advance measurement
clock: 1000000 mtime ticks -> rdcycle delta 149954205 (ratio 149 rdcycle units per tick)
baseline: mtime0=0x1005e7 mtime1=0x100843 delta=604
write/readback: written=0x100000000 readback=0x1000001f9 delta=505
write took effect (readback >= written, delta <= 100000): yes
advance: 0x10000236a 0x100006893 0x10000a7e9 0x10000e65a
advance samples strictly increasing: yes
advance samples all >= written base: yes
RESULT: PASS
```

## Limits

- Emulator, not silicon: the behavior measured is QEMU 8.2.2's model
  of the `virt` board. `mtime` writability is an implementation
  detail in the privileged specification (the register must be
  readable, and writable on platforms that expose it); a physical
  core or a different CLINT may ignore the write, and the write
  being honored is this emulator's behavior, not a portable
  guarantee.
- The delta between the write and the immediate readback is host
  timing, not a property of the guest: on QEMU without icount
  `mtime` is driven by the host wall clock, so the 505-690 tick
  delta will differ on another host or under load.
- Single hart, no interrupts, no other code running: the module
  measures the `mtime` store/readback path and the advancement
  property of this one register, nothing about multi-hart or
  timer-interrupt behavior.
