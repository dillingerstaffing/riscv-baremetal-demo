<!-- PROOF-HEADER
Checks: 18
Mismatches: 0
Checksum: 0x1000c43ed0ebf94d
Throughput: 3 runs, both scause values identical across runs
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: scause INTERRUPT-bit probe (backlog item 125)

## What was built

A bare-metal RV64 module that delegates the supervisor timer
interrupt (`mideleg` bit 5) and the load page fault (`medeleg` bit 13)
to S-mode on QEMU 8.2.2 (`virt`), then triggers both in S-mode and
records `scause`:

- Fault phase: a 4-byte `ld` from the deliberately unmapped VA
  `0x40000000` (Sv39 walk dies at the root lookup). Expected:
  `scause = 13` (bit 63 clear, load page fault per the privileged
  spec's `scause` encoding: INTERRUPT = bit 63, code = low bits),
  `stval` = the faulting address.
- Timer phase: `stimecmp` armed 10000 ticks ahead, SIE then enabled.
  Expected: `scause = 0x8000000000000005` (bit 63 set, cause 5 =
  supervisor timer interrupt).

The two phases are separated by privilege state: the fault runs with
SIE clear (exceptions trap regardless of SIE), and the timer can only
fire after SIE is set, so the two `scause` values cannot be confused.

Files: `scb_main.c`, `scb_trap.S`, `README.md`. Raw QEMU logs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(byte-for-byte identical across three boots, modulo the `timeout`
kill line's pid). Build log: `bench-logs/build.log`.

## Measured scause values (all 3 runs)

```
fault: s_traps=1 scause=0xd stval=0x40000000 sepc=0x80000306 after_ld-4=0x80000306
timer: s_traps=2 scause=0x8000000000000005 stimecmp=0xffffffffffffffff unexpected=0 m_traps=0
RESULT: PASS
```

- `scause = 0xd`: bit 63 clear, code 13 = load page fault. `stval`
  equals the faulting VA `0x40000000`, and `sepc` equals the address
  of the faulting `ld` (verified against an in-assembly label:
  `after_ld - 4`).
- `scause = 0x8000000000000005`: bit 63 set, code 5 = supervisor
  timer interrupt. The handler disarmed the source (`stimecmp` reads
  back all-ones), exactly 2 S-mode traps total, 0 unexpected traps,
  0 M-mode traps.
- Delegation readbacks: `medeleg` write `0x2000` reads back
  `0x2000` exactly; `mideleg` zero-write reads back `0x1444` (QEMU
  ORs the hypervisor interrupt bits in after every write, the same
  forced set `src/mideleg-route` measured), bit-5 write reads back
  `0x1464` (forced set plus exactly bit 5).
- `satp` reads back `0x8000000000080004` (MODE=8, Sv39, ASID 0).

Checksum `0x1000c43ed0ebf94d` is FNV-1a-64 over the 16 bytes of the
two recorded `scause` values (`0xd` then `0x8000000000000005`, each 8
bytes little-endian), identical in all 3 runs.

## Build log (genuine)

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/scause-bit/scb_trap.S -o src/scause-bit/scb_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/scause-bit/scb_main.c -o src/scause-bit/scb_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o scause-bit.elf src/boot.o src/uart.o src/scause-bit/scb_trap.o src/scause-bit/scb_main.o
```

## Raw run outputs (genuine, all 3 runs)

Run 1:
```
scause-bit: scause INTERRUPT-bit probe (backlog item 125)
sstc: stce=1
medeleg: write=0x2000 readback=0x2000
mideleg: write=0x0 readback=0x1444 write=0x20 readback=0x1464
m-mode: satp=0x8000000000080004 (MODE=8 ASID=0)
m-mode: entering S-mode
s-mode: entered, translation active
fault: s_traps=1 scause=0xd stval=0x40000000 sepc=0x80000306 after_ld-4=0x80000306
timer: s_traps=2 scause=0x8000000000000005 stimecmp=0xffffffffffffffff unexpected=0 m_traps=0
RESULT: PASS
done
```

Runs 2 and 3 print the identical lines above (verified with `diff`:
byte-for-byte identical apart from the `timeout` process's own
termination notice, which carries a varying pid and is not program
output). All three runs print `RESULT: PASS`; no `FAIL` line appears
in any log.

## Checks performed (18, 0 mismatches)

M-mode setup: Sstc present (`menvcfg.STCE` sticks); the three table
pages 4 KiB aligned; `medeleg` readback `0x2000`; `mideleg` readback
equals the forced set with only bit 5 added; `satp` MODE=8 and PPN
equals the root page. S-mode: Sstc flag carried from M-mode; fault
phase trap count 1, `scause` 13, `stval` `0x40000000`, `sepc` equals
the faulting `ld` address; timer phase total trap count 2, `scause`
`0x8000000000000005`, 0 unexpected traps, 0 M-mode traps, `stimecmp`
disarm readback all-ones.

## Limits, stated honestly

- These are emulator measurements: QEMU 8.2.2 TCG on the `virt`
  machine, not silicon. The `scause` encoding (INTERRUPT = bit 63,
  code 5 = supervisor timer interrupt, code 13 = load page fault)
  comes from the RISC-V privileged specification; the values
  observed are QEMU's implementation of it.
- The `mideleg` forced set `0x1444` is a QEMU behavior (hypervisor
  interrupt bits ORed in after each write); on real hardware the
  writable set is defined by the hart's implemented extensions.
- The faulting `ld` is 4 bytes because its destination `t1` (x6) is
  not a compressible register; the `sepc + 4` resume depends on that
  exact encoding, verified against the in-assembly label.
