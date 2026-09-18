<!-- PROOF-HEADER
Checks: 33
Mismatches: 0
Checksum: 0xfb7bd8f9f90b987b
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: pmp entry-priority first-match (backlog item "riscv pmp-priority-first-match")

## What was built

`src/pmp-priority/`: a bare-metal RISC-V program that proves PMP
matching is priority-ordered, the lowest-numbered matching entry
wins, by programming TWO entries to match the same 4 KiB scratch
page with conflicting permissions and swapping them between two
phases. Phase A (entry 0 = allow, entry 1 = deny): an S-mode `lbu`
of a canary from the page completes with no trap. Phase B (entry 0
= deny, entry 1 = allow): the same `lbu` traps in M-mode with
`mcause = 0x5` (load access fault), `mepc` at the faulting load and
`mtval` at the scratch page.

Why NAPOT, not TOR (backlog correction, recorded honestly): the
backlog text says TOR, but TOR entries are half-open ranges that
partition the address space, so two TOR entries can never both
match one address. A true priority test needs two entries that both
MATCH the same address with conflicting permissions, so entries 0
and 1 are NAPOT over the same 4 KiB page with the same `pmpaddr`
value (`(pa >> 2) | 0x1ff`). The mechanism under test is entry
priority, not address matching, which distinguishes this module
from the done `src/pmp-tor` (match semantics) and
`src/pmp-lock-bit` (locking).

Locked PMP entries cannot be reprogrammed, so the test entries are
UNLOCKED; unlocked entries do not apply to M-mode, so the probe
load runs in S-mode. M-mode drops to S-mode via `sret` with `satp`
staying Bare (no page tables needed, so the probe address is the
physical page address), the S-mode payload writes a canary and
reads it back with `lbu`, then `ecall` returns to M-mode.
`medeleg` stays 0, so the phase-B fault traps in M-mode. With any
PMP entry implemented, an S-mode access with no matching entry
FAILS (default deny), so a higher-numbered broad allow entry
(entry 2, NAPOT R|W|X over `[0x80000000, 0x100000000)`,
`pmpaddr2 = 0x2fffffff`) keeps S-mode code fetch and the control
load working outside the scratch page; entries 0/1 keep priority
over it for the scratch page. Every `pmpcfg`/`pmpaddr` register is
read back and verified in both phases.

Phase A runs with `pmpcfg0 = 0x1f181b` (entry 0 NAPOT R|W allow,
entry 1 NAPOT no-perms deny, entry 2 NAPOT R|W|X allow): the S-mode
`lbu` of the canary completes, returns `0xa5`, zero traps; the
`ecall` returns to M-mode (cause 9, not delegated). Phase B
rewrites `pmpcfg0` (unlocked, writable) to `0x1f1b18` (entry 0
deny, entry 1 allow, entry 2 unchanged): the same S-mode `lbu`
traps in M-mode with `mcause = 0x5`, `mepc` at the faulting load,
`mtval` at the scratch page; the handler records it and redirects
to the report. Each phase first loads a control byte covered ONLY
by the broad allow entry (completes in both phases, proving the
phase-B trap comes from the priority swap and not from the load
path itself). A quiet spin window moves neither trap counter, and
the boot PMP config and `medeleg` are restored and reported at the
end. Five files, sharing only `src/boot.S` and `src/uart.c` with
the other demos. Exactly one mechanism is under test: PMP entry
priority on two matching entries.

- `pp_trap.S`: M-mode trap entry (records `mcause`/`mepc`/
  `mstatus`/`mtval` in `m_regs`; the only M-mode traps possible
  are the phase-A `ecall` and the phase-B access fault, so the
  handler loads the armed continuation from its save area into
  `mepc`, sets `mstatus.MPP` to M-mode, and `mret`s into it) and
  S-mode trap entry (safety net only; `medeleg` is 0, so no S-mode
  trap can be delegated). Zero continuation parks the hart.
- `pp_main.c`: two-phase driver. Computes the NAPOT `pmpaddr` for
  the scratch page, programs the three PMP entries and verifies
  every readback, drops M -> S via `sret` with `satp` Bare, swaps
  `pmpcfg0` between phases, restores the boot PMP config,
  publishes every measured value, runs the checks, computes an
  FNV-1a checksum over the verdict values, and reports PASS/FAIL.
  On PASS it writes `0x5555` to the virt test-device finisher so
  the QEMU process exits 0; on FAIL it parks the hart.

## Build log

From `bench-logs/build.log` (riscv64-unknown-elf-gcc 13.2.0,
`-O2`, `boot.o` first in link order):

```
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/pmp-priority/pp_trap.S -o src/pmp-priority/pp_trap.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/pmp-priority/pp_main.c -o src/pmp-priority/pp_main.o
/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o pmp-priority.elf src/boot.o src/uart.o src/pmp-priority/pp_trap.o src/pmp-priority/pp_main.o
```

Zero warnings (the `ld` RWX LOAD-segment note is the standard
benign one this link script produces for every module). The build
hit two real issues, both fixed in the shipped source: (1) GCC
folded the broad-window base constant into the PC-relative address
materialization of the payload functions, producing an `auipc`
with a `-0x80000000` addend that overflows the 20-bit immediate
and fails the link; the window test is now a `noinline` helper so
the caller materializes the address with a plain `auipc`. (2) The
first passing run reported FAIL on three checks because the
restore step reused the phase-B readback slots as its write
targets, clobbering the values the phase-B checks read; the
restore now uses its own slots.

## Run output (QEMU 8.2.2, `qemu-system-riscv64 -machine virt -nographic -bios none -kernel pmp-priority.elf`)

From `bench-logs/run1.log` (runs 1-3 byte-identical, all exit 0):

```
pmp-priority: PMP entry-priority first-match test
m-mode: boot medeleg=0x0 pmpcfg0=0x0 pmpaddr0=0x0 pmpaddr1=0x0 pmpaddr2=0x0
m-mode: phase-A pmpcfg0=0x1f181b pmpaddr0=0x20000dff pmpaddr1=0x20000dff pmpaddr2=0x2fffffff
m-mode: satp=0x0 (Bare)
m-mode: entering S-mode (phase A, entry 0 allows)
m-mode: phase-B pmpcfg0=0x1f1b18 pmpaddr0=0x20000dff pmpaddr1=0x20000dff pmpaddr2=0x2fffffff
pmp-priority: PMP entry-priority first-match test
boot: medeleg=0x0 restored readback=0x0
boot: pmpcfg0=0x0 pmpaddr0=0x0 pmpaddr1=0x0 pmpaddr2=0x0
restore: pmpcfg0 readback=0x0 pmpaddr0=0x0 pmpaddr1=0x0 pmpaddr2=0x0
scratch: pa=0x80003000 napot=0x20000dff control=0x80001a8b
pmpA: cfg0=0x1f181b addr0=0x20000dff addr1=0x20000dff addr2=0x2fffffff
pmpB: cfg0=0x1f1b18 addr0=0x20000dff addr1=0x20000dff addr2=0x2fffffff
pA: s_traps=0 canary=0xa5 control=0x5a
pA: m_traps=1 mcause=0x9 mepc=0x80000320 mstatus_mpp=1
pB: m_traps=2 mcause=0x5 mepc=0x80000362 expected=0x80000362 mtval=0x80003000 mstatus_mpp=1 control=0x5a s_traps=0
quiet: m_traps=2 s_traps=0
checksum=0xfb7bd8f9f90b987b
RESULT: PASS (checks=33)
```

## What the numbers mean

- `pmpaddr0 = pmpaddr1 = 0x20000dff`: both test entries carry the
  same NAPOT value, `(0x80003000 >> 2) | 0x1ff`, so entries 0 and
  1 match exactly the same 4 KiB page at `0x80003000`; the only
  difference between them is the permission byte. `pmpaddr2 =
  0x2fffffff` is the broad NAPOT R|W|X allow over
  `[0x80000000, 0x100000000)`. The `pmpcfg0` readbacks
  `0x1f181b` (phase A: entry 0 allow, entry 1 deny) and `0x1f1b18`
  (phase B: swapped) are exactly the written values, so the
  trap/no-trap pair ran under the intended configurations.
- Phase A (entry 0 allows): zero S-mode traps, the `lbu` returned
  the canary `0xa5`, the control load returned `0x5a`; the M-mode
  handler saw exactly one trap and it was the phase-A return
  `ecall` (`mcause=0x9`), arriving from S-mode
  (`mstatus.MPP=1`). Entry 0's allow won the priority match.
- Phase B (entry 0 denies): exactly one further M-mode trap,
  `mcause=0x5` (load access fault), `mepc=0x80000362` exactly at
  the faulting `lbu` instruction, `mtval=0x80003000` exactly the
  scratch page, arriving from S-mode; the S-mode trap count stayed
  0 and the control load still returned `0x5a`. Entry 0's deny won
  the priority match even though entry 1 allowed the page.
- The 2,000,000-iteration quiet window moved neither counter.
- The boot PMP config (`pmpcfg0=0x0`, all `pmpaddr` zero) and
  `medeleg=0x0` were restored and the readbacks match.
- FNV-1a `0xfb7bd8f9f90b987b` over the twenty-one verdict values
  (both phases' `pmpcfg0`/`pmpaddr` readbacks, scratch page
  address, NAPOT encoding, per-phase trap counts, causes, PCs,
  trap values, canary and control values) is byte-identical across
  all three runs.

## Verification record

- 33 checks, 0 failures, verdict PASS, QEMU exit code 0 on all
  three runs; `cmp` of the three raw logs is empty.
- The scratch page is verified 4 KiB aligned, the NAPOT encoding
  is recomputed as `(pa >> 2) | 0x1ff` and checked against the
  readback, the page is verified inside entry 2's window (so both
  entries 0/1 AND entry 2 match it, and priority alone decides),
  and the control byte is verified off the scratch page.
- The faulting load address was taken with a numeric assembler
  local label (`la t1, 0f`) at the faulting instruction itself, so
  `mepc` is checked against the exact instruction address rather
  than a guessed constant. The M-mode handler never advances
  `mepc` past the faulting load, so no instruction-length
  arithmetic exists to get wrong.
- `satp` readback is 0 (Bare) for the whole run; no page tables
  exist, so the PMP check is the only access control in play.
- Toolchain: riscv64-unknown-elf-gcc 13.2.0
  (`~/workspace/toolchains/ubuntu-rv64`), QEMU 8.2.2
  (`~/workspace/qemu`), `-bios none`, single hart.
