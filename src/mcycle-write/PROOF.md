<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mcycle write/readback/advance measurement (backlog item 110)

## What was built

`src/mcycle-write/`: a bare-metal RISC-V program that writes a known
constant to the M-mode cycle counter `mcycle`, reads it back
immediately, and publishes the write/readback/delta triple, then
verifies the counter advances strictly from the written base. One
file, sharing only `src/boot.S` and `src/uart.c` with the other
demos. Exactly one mechanism is under test: whether a `csrw` to
`mcycle` takes effect on this machine and whether the counter keeps
advancing afterward.

- `mcw_main.c`: reads `mcycle` twice (baseline, the second must be
  larger), writes `0x100000000` with `csrw`, reads back immediately
  and prints written/readback/delta, then takes four further
  readbacks that must each be strictly larger than the previous
  sample and none below the written base. The write verdict is not
  assumed: the unsigned delta `readback - written` is compared
  against a bound of 100000 host-tick units, so an ignored write
  (readback below the written value, or far above it) prints the
  actual values and fails the check instead of passing silently. On
  PASS it shuts the machine down via the virt test-device finisher
  so the QEMU process exit code (0) reflects the verdict; on FAIL it
  parks the hart and the bench harness observes the timeout exit
  status (124) plus the RESULT line.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make mcycle-write.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none
-kernel mcycle-write.elf` (or `make run-mcycle-write`).

Toolchain: riscv64-unknown-elf-gcc 13.2.0
(`/usr/bin/riscv64-unknown-elf-gcc`, Debian), QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- The program is the only code running; no S-mode, no interrupts
  enabled, no PLIC or CLINT involvement. The only CSR under test is
  `mcycle`, accessed from M-mode.

## Sequence and controls

1. Read `mcycle` twice; the second read must be strictly larger.
   This confirms the counter is live before the write (on QEMU
   without icount, `mcycle` is driven by the host virtual clock, so
   the counter ticks with wall time between any two reads).
2. Write `0x100000000` to `mcycle` with `csrw`.
3. Read back immediately and print the written value, the readback,
   and the unsigned delta `readback - written`. The readback must be
   >= the written value and within 100000 units of it; a smaller
   readback (the write did not stick) wraps the unsigned delta to a
   huge value and fails the bound check, with the actual numbers
   printed.
4. Take four further readbacks. Each must be strictly larger than
   the previous sample, and none may fall below the written base.

## Measured results

The write is honored on QEMU 8.2.2: every run read back the written
value plus a few thousand host-tick units (the time the emulator
needed between the `csrw` and the `csrr` plus host-clock granularity),
and the counter advanced strictly from there. Write/readback/delta
triples:

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| baseline `mcycle0` / `mcycle1` | `0x3ff74b042d1` / `0x3ff74b05da7` | `0x3ff7a7530be` / `0x3ff7a754ba3` | `0x3ff7fb47364` / `0x3ff7fb48e67` |
| baseline delta | 6870 | 6885 | 6915 |
| written | `0x100000000` | `0x100000000` | `0x100000000` |
| readback | `0x100001e1e` | `0x100001d5b` | `0x100001cc5` |
| write/readback delta | 7710 | 7515 | 7365 |
| write took effect | yes | yes | yes |
| advance samples | `0x1001276fe 0x100161403 0x100176871 0x10018ba1e` | `0x100229e5d 0x10024ceee 0x100262569 0x100277c02` | `0x10011fbd4 0x100140a1e 0x100155fe5 0x10016b354` |
| advance strictly increasing | yes | yes | yes |
| advance all >= written base | yes | yes | yes |
| verdict | PASS | PASS | PASS |

The three run logs are NOT byte-identical (md5: run1
`6638270c`, run2 `5925d32a`, run3 `849031fc`): baseline values and
deltas vary with the host clock, as expected for a time-driven
counter. Everything structural is identical across runs: the banner,
the written constant, the "yes" answers, the verdicts. The
write/readback delta sits around 7.4-7.7k host-tick units on all
three runs, consistent with the time between the write and the
immediate read on this host.

## Raw QEMU output

### Run 1 (bench-logs/run1.log)

```
mcycle-write: mcycle write/readback/advance measurement
baseline: mcycle0=0x3ff74b042d1 mcycle1=0x3ff74b05da7 delta=6870
write/readback: written=0x100000000 readback=0x100001e1e delta=7710
write took effect (readback >= written, delta <= 100000): yes
advance: 0x1001276fe 0x100161403 0x100176871 0x10018ba1e
advance samples strictly increasing: yes
advance samples all >= written base: yes
RESULT: PASS
```

### Run 2 (bench-logs/run2.log)

```
mcycle-write: mcycle write/readback/advance measurement
baseline: mcycle0=0x3ff7a7530be mcycle1=0x3ff7a754ba3 delta=6885
write/readback: written=0x100000000 readback=0x100001d5b delta=7515
write took effect (readback >= written, delta <= 100000): yes
advance: 0x100229e5d 0x10024ceee 0x100262569 0x100277c02
advance samples strictly increasing: yes
advance samples all >= written base: yes
RESULT: PASS
```

### Run 3 (bench-logs/run3.log)

```
mcycle-write: mcycle write/readback/advance measurement
baseline: mcycle0=0x3ff7fb47364 mcycle1=0x3ff7fb48e67 delta=6915
write/readback: written=0x100000000 readback=0x100001cc5 delta=7365
write took effect (readback >= written, delta <= 100000): yes
advance: 0x10011fbd4 0x100140a1e 0x100155fe5 0x10016b354
advance samples strictly increasing: yes
advance samples all >= written base: yes
RESULT: PASS
```

## Limits

- Emulator, not silicon: the behavior measured is QEMU 8.2.2's model
  of the `virt` board. `mcycle` is WARL in the privileged
  specification; a physical core may ignore the write, coerce it, or
  honor it, and may tick the counter at a fixed instruction rate
  rather than with host time. The write being honored is this
  emulator's behavior, not a portable guarantee.
- The delta between the write and the immediate readback is host
  timing, not a property of the guest: on QEMU without icount the
  cycle counter is driven by the host virtual clock, so the
  ~7.5k-unit delta will differ on another host or under load.
- Single hart, no interrupts, no other code running: the module
  measures the CSR write/readback path and the advancement
  property of this one counter, nothing about multi-hart or
  supervisor-visible (`cycle`) counter behavior.
