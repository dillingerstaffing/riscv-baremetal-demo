<!-- PROOF-HEADER
Checks: 17
Mismatches: 0
Checksum: 0xfbef9753a3301da1
Environment: QEMU 8.2.2
Verdict: PASS
-->
# Proof: sip.SEIP is read-only for S-mode (backlog item riscv sip-seip-write)

## What was built

`src/sip-seip-write/`: a bare-metal RISC-V program that measures what
an S-mode write of all ones to `sip` does to bit 9 (SEIP), on the
QEMU `virt` board. Four files, sharing only `src/boot.S` and
`src/uart.c` with the other demos. Exactly one mechanism is under
test: SEIP reports the pending state of the supervisor external
interrupt and is read-only for S-mode, so the all-ones write must
leave it exactly as it was.

- `seipw.h`: bit definitions, the M-mode trap save-area layout, and
  the module contract.
- `seipw_trap.S`: the only trap vector. M-mode, direct. Entry swaps
  `t0` with `mscratch`, bumps a trap count, records `mcause`/`mepc`,
  and advances `mepc` past the trapping instruction (2 or 4 bytes) so
  a surprise trap cannot loop silently. No stack is used.
- `seipw_main.c`: M-mode boot (UART, counting vector, timer
  disarmed, Sstc probe, one PMP NAPOT R/W/X entry, `mcounteren`,
  `mideleg` bit 1 only, then `mret` to S-mode) and the S-mode
  payload: record the `sip` readback, write all ones, read back and
  require SEIP unchanged with SSIP stuck (proving the write
  executed), require the M-mode trap count to stay 0, write zero,
  read back and require the writable bits clear with SEIP still
  unchanged. Every expectation is an in-program check; `RESULT:
  PASS` is printed only when every check held. The
  verdict-relevant values feed a 64-bit FNV-1a digest printed as the
  last data line. On PASS the machine is shut down through the virt
  test-device finisher (QEMU exits 0); on FAIL the hart parks without
  touching the finisher.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make CROSS=riscv-none-elf- sip-seip-write.elf` (added to
`all` in the Makefile; the `CROSS` override selects the xPack
toolchain on this machine).
Run: `timeout 30 qemu-system-riscv64 -machine virt -nographic -bios
none -kernel sip-seip-write.elf` (or `make run-sip-seip-write`),
with `LD_LIBRARY_PATH` pointing at the QEMU 8.2.2 libraries, under
`timeout` so a parked-hart FAIL is observable as exit status 124.

Toolchain: `riscv-none-elf-gcc` (xPack GNU RISC-V Embedded GCC)
15.2.0, `-march=rv64imac_zicsr`. QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: hart 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the module drops to S-mode via `mret`.
- `mideleg` bit 1 (SSI) is the only delegation. This is deliberate,
  and it is the one place the module deviates from a pure
  delegate-nothing design: the first bring-up run left `mideleg`
  untouched and the S-mode all-ones write read back `0x0`, with
  SSIP dropped too. On this QEMU, S-mode `sip` writes are dropped
  entirely unless the bit is delegated (the same rule the
  `sip-ssip` sibling measured in M-mode), so without delegation the
  "SEIP ignored" claim would be confounded with "write dropped".
  Delegating SSI makes the write observable while keeping the
  control intact: `sie` and `sstatus.SIE` stay clear, so the pended
  SSIP is never taken, and SEI (bit 9) is not delegated, so the SEIP
  bit under test keeps its M-mode trap path.
- `mideleg` reads back `0x1446`: this QEMU's reset value keeps bits
  2, 6, 8, 10 set (virtualization-related, unused by this
  single-hart run; the `sip-stip-write` sibling measured the same
  behavior as `0x1464` after setting bit 5). The module checks the
  bits that matter: SSI delegated, STI and SEI not.
- The machine timer comparator is parked at all-ones and
  `menvcfg.STCE` sticks, so Sstc is present; STIP is still
  legalized away on this hart (matches the `sip-stip-write`
  sibling), which is reported, not assumed.

## Results

Three QEMU 8.2.2 runs, exit code 0 each (finisher shutdown), output
byte-identical across runs (md5
`239df0f197511ef9b29bf7fcebd8cb57` for all three logs).

| step | run1 | run2 | run3 |
|---|---|---|---|
| `menvcfg.STCE` probe | sticks (present) | sticks (present) | sticks (present) |
| `mideleg` readback | 0x1446 (SSI took, STI/SEI clear) | 0x1446 | 0x1446 |
| `sie` at S-mode entry | 0x0 | 0x0 | 0x0 |
| `sstatus.SIE` at S-mode entry | clear | clear | clear |
| `sip` before the write | 0x0 (SEIP clear) | 0x0 | 0x0 |
| `sip` after `csrw sip, all-ones` | 0x2 (SSIP set, SEIP clear) | 0x2 | 0x2 |
| M-mode trap count after the writes | 0 | 0 | 0 |
| `sip` after `csrw sip, zero` | 0x0 | 0x0 | 0x0 |
| M-mode trap count at end | 0 | 0 | 0 |
| checks / mismatches | 17 / 0 | 17 / 0 | 17 / 0 |
| FNV-1a checksum of verdict values | 0xfbef9753a3301da1 | 0xfbef9753a3301da1 | 0xfbef9753a3301da1 |
| RESULT | PASS | PASS | PASS |

The 17 checks: `mtvec` direct mode; `pmpcfg0` opened NAPOT R/W/X;
Sstc present; `mideleg` exactly SSI-delegated; SIE clear; `sie`
zero; SEIP clear before the write (premise); SEIP unchanged by the
all-ones write; SEIP clear after it; SSIP stuck (write executed);
readback equals the exact legalized value; trap count 0 after the
writes; SSIP clear after the zero write; STIP clear after it; SEIP
unchanged by the zero write; readback equals the exact legalized
value; trap count 0 at end.

What the readbacks ground:

- `sip = 0x2` after the S-mode all-ones write: bit 1 (SSIP) is
  software-writable and stuck, which proves the write executed;
  bit 9 (SEIP) reads back exactly as before (0), which is the
  read-only claim; bit 5 (STIP) is legalized away on this hart.
  The exact-equality assertions pin the legalized value both ways:
  after all-ones it must be `before | 0x2`, after zero it must be
  `before & ~0x22`.
- Trap count 0 with `sie`/`SIE` clear and the counting M-mode
  handler installed means no trap was involved in producing these
  readbacks; the S-mode CSR writes neither faulted nor delivered.
- The checksum covers the two write values, the three readbacks,
  the trap count, the entry `sie`, and the SSIP/STIP-stuck flags.
  It is identical across runs because every measured value is
  identical.

## What was verified, and what was not

Verified: on QEMU 8.2.2, an S-mode `csrw sip, -1` with SSI
delegated leaves SEIP (bit 9) unchanged while SSIP sticks, and a
`csrw sip, 0` clears SSIP with SEIP still unchanged; no trap fires
in either direction; three runs were byte-identical (digest
`0xfbef9753a3301da1`).

Not verified: behavior on real silicon. These numbers come from the
QEMU 8.2.2 CSR model, not from hardware. Also not verified: the
SEIP=1 premise path (no external interrupt is asserted on the quiet
board, so SEIP reads 0 throughout; the module aborts honestly
rather than forcing the premise if SEIP is ever set at boot), and
any STIP software-writability (the write is legalized away here).

## Limits

- The module assumes the `mideleg` write, the PMP opening, the SIE
  clear, and the `sie` zero state hold exactly as written (each
  checked via readback; the run fails loudly if any does not).
- The `mideleg` reset value `0x1444` (bits 2, 6, 8, 10) is this
  emulator's choice and is not clearable by the module's write; the
  check asserts only the bits that matter (SSI set, STI/SEI
  clear).
- The "write executed" proof rests on SSIP sticking, which on this
  QEMU requires the SSI delegation; the delegation is the minimal
  change that makes the write observable, and the SEI bit under
  test is never delegated.

## Reproduction

```
make CROSS=riscv-none-elf- sip-seip-write.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel sip-seip-write.elf
```

## Build log

```
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sip-seip-write/seipw_trap.S -o src/sip-seip-write/seipw_trap.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sip-seip-write/seipw_main.c -o src/sip-seip-write/seipw_main.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o sip-seip-write.elf src/boot.o src/uart.o src/sip-seip-write/seipw_trap.o src/sip-seip-write/seipw_main.o
```

(The linker prints its usual `LOAD segment with RWX permissions`
warning, as for every module in this repo.)

## Raw run output (run1; run2 and run3 are byte-identical)

```
sip-seip-write: S-mode sip.SEIP read-only probe
Sstc probe: menvcfg.STCE sticks (present)
mideleg=0x1446
dropping to S-mode
in S-mode: only SSI delegated (mideleg bit 1); SEI stays in M-mode
sie at entry=0x0
sip before=0x0
writing all-ones to sip from S-mode
sip after all-ones=0x2
SSIP-stuck=1 STIP-stuck=0
NOTE: STIP legalized away on this hart (matches the sip-stip-write sibling); SSIP is the write-executed proof
M-mode trap count after the writes=0 (expect 0)
writing zero to sip from S-mode
sip after zero=0x0
VERDICT sip_before=0x0 sip_after=0x2 sip_zeroed=0x0 traps=0 checksum=0xfbef9753a3301da1
checks=17 fails=0
RESULT: PASS
```
