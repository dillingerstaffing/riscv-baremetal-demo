<!-- PROOF-HEADER
Checks: 17
Mismatches: 0
Checksum: n/a
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mideleg bit-1 supervisor-software-interrupt routing (backlog item "riscv mideleg-ssip-route")

## What was built

`src/mideleg-ssip-route/`: a bare-metal RISC-V program that sets
`mideleg` to delegate only the supervisor software interrupt (bit 1,
readback-verified), drops to S-mode, and checks that pending the
interrupt's own source (`mip.SSIP`, bit 1) traps to S-mode with
`scause = 0x8000000000000001`, while a pending CLINT `msip` takes no
trap of either kind. Four files, sharing only `src/boot.S` and
`src/uart.c` with the other demos. Exactly one mechanism is under
test: routing of the supervisor software interrupt through
`mideleg` bit 1.

- `mssr_trap.S`: M-mode trap entry (records `mcause`/`mepc` in
  `m_regs`; no M-mode trap is expected, so the handler only records)
  and S-mode trap entry (records `scause`/`sepc`/the `sip` value at
  entry in `s_regs`, clears `sip.SSIP` so the level-triggered source
  fires exactly once, raises the done flag).
- `mssr_main.c`: installs direct-mode `mtvec`/`mscratch` and
  `stvec`/`sscratch`, records the boot `mideleg` value, programs the
  selective delegation with readback checks, opens the address space
  to S-mode with one PMP NAPOT entry, disarms `mie`/`mstatus.MIE`,
  drops to S-mode for the control poll, the CLINT `msip` negative
  control, the `SSIP` pend-and-trap, and the quiet window, and prints
  `RESULT: PASS` only when all 16 checks held.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make mideleg-ssip-route.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel mideleg-ssip-route.elf`
(or `make run-mideleg-ssip-route`).

Toolchain: riscv64-unknown-elf-gcc 13.2.0, QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`.
- The program is the only code running. All test phases run in S-mode
  after a `sret` drop (one PMP NAPOT entry opens the whole address
  space; `stvec`/`sscratch` installed beforehand). `medeleg` is zero
  throughout, so no exception is ever delegated. `mie` and
  `mstatus.MIE` are clear throughout the S-mode phases, so no M-mode
  interrupt can fire; `sie.SSIE` is the only interrupt enable set.
- The CLINT `msip` register for hart 0 is accessed as a 32-bit
  register at `0x02000000` (this QEMU's CLINT model only accepts
  4-byte accesses to `msip`; see the defect note in
  `src/msip/PROOF.md`, backlog item 70).

## Sequence and controls

1. Record boot-time `mideleg` (`0x1444`).
2. Write `0` to `mideleg`, read back: exposes the forced set
   (`0x1444`). Write `0x2`, read back: must equal the forced set
   with only bit 1 added (`0x1446`); bit 9 must read back clear, so
   only the supervisor software interrupt is delegated. Write `0`
   to `medeleg`, read back `0x0`.
3. Control: in S-mode with `sie.SSIE` and `sstatus.SIE` set and
   nothing pending, poll; require 0 S-mode and 0 M-mode traps.
4. Negative control: set the CLINT `msip` (reads back 1), poll with
   `SIE` on while it stays pending; require 0 traps of either kind.
   Clear `msip`, read back 0.
5. Pend `SSIP` via `csrs sip` with `SIE` off; `sip` must read back
   `0x2`. Set `sstatus.SIE`; the pending interrupt is taken at the
   next instruction boundary.
6. In S-mode: require exactly one trap, `scause =
   0x8000000000000001`, `sepc` equal to the address of the
   interrupted instruction (the `nop` right after the `SIE` enable,
   captured with an in-assembly local label), `sip` at handler entry
   showing `SSIP` set, `sip` reading `0x0` after the handler cleared
   it, and 0 M-mode traps.
7. Quiet window: poll with `SIE` on; require the trap counts
   unchanged. Print `RESULT: PASS` only if all 16 checks held.

## Measured results

All quantities below are identical across the three runs
(byte-identical logs modulo the harness timeout line):

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| boot `mideleg` | `0x1444` | identical | identical |
| `mideleg` write `0x0` readback | `0x1444` | identical | identical |
| `mideleg` write `0x2` readback | `0x1446` | identical | identical |
| `medeleg` write `0x0` readback | `0x0` | identical | identical |
| control: S-mode / M-mode traps | 0 / 0 | identical | identical |
| negctl: `msip` readback after set | 1 | identical | identical |
| negctl: S-mode / M-mode traps while pending | 0 / 0 | identical | identical |
| negctl: `msip` after software clear | 0 | identical | identical |
| `sip` readback after pending SSIP | `0x2` | identical | identical |
| SSI: S-mode traps / `scause` | 1 / `0x8000000000000001` | identical | identical |
| SSI: `sepc` / interrupted-instruction address | `0x8000040e` / `0x8000040e` | identical | identical |
| SSI: `sip` at handler entry | `0x2` | identical | identical |
| SSI: `sip` after handler / M-mode traps | `0x0` / 0 | identical | identical |
| quiet: S-mode / M-mode traps | 1 / 0 | identical | identical |
| final `mideleg` programmed value | `0x1446` | identical | identical |
| verdict | PASS | PASS | PASS |

The 17 checks: (1) `mideleg` write changed only bit 1,
(2) `mideleg` bit 9 clear, (3) `medeleg` readback zero,
(4) control poll trap-free, (5) `msip` set readback 1,
(6) pending `msip` routes no trap of either kind,
(7) `msip` stayed set through the poll, (8) `msip` software clear
reads 0, (9) `sip` readback `0x2` after pending SSIP,
(10) SSI trap arrived within budget, (11) exactly one S-mode trap,
(12) `scause`, (13) `sepc` equals the interrupted-instruction
address, (14) `sip` at handler entry showed SSIP,
(15) `sip` cleared by the handler, (16) 0 M-mode traps,
(17) quiet window counts unchanged. Zero failures on all three runs.

## The mideleg readback

Writing `0x2` to `mideleg` reads back `0x1446`, not `0x2`.
This is not a failed write: QEMU 8.2.2's `rmw_mideleg64`
(`target/riscv/csr.c`) computes
`mideleg = (old & ~mask) | (new & mask)` with
`mask = delegable_ints`, then ORs `HS_MODE_INTERRUPTS` back in
whenever the hypervisor extension is present. On this hart the
hypervisor extension is present, so bits 2, 6, 10, 12
(`VSSIP`/`VSTIP`/`VSEIP`/`SGEIP`) are forced to 1 on every write
and read back 1 regardless of the written value. The observed
arithmetic matches that code path exactly: boot `0x1444`, write
`0x0` reads back `0x1444`, write `0x2` reads back
`0x2 | 0x1444 = 0x1446`. The program's check therefore verifies
the property the backlog item actually needs: among the
software-writable bits, the write set only bit 1 (readback equals
the zero-write readback with bit 1 added), and bit 9 reads back
clear.

## The msip non-routing

The backlog item as written names the CLINT `msip` as the interrupt
source. A pre-build experiment measured that on QEMU 8.2.2 a CLINT
`msip` set drives `mip.MSIP` (bit 3, the machine software interrupt,
cause 3), which `mideleg` bit 1 does not delegate: with `mie.MSIE`
and `mstatus.MIE` armed it traps to M-mode with
`mcause = 0x8000000000000003` and zero S-mode traps. The supervisor
software interrupt's own source is `mip.SSIP` (bit 1); that is what
this module pends, and step 4 above re-verifies the non-routing
inside the run (pending `msip`, `SIE`/`SSIE` on, 0 traps of either
kind, `msip` still reading 1 throughout the poll).

## Raw QEMU output

### Run 1 (bench-logs/run1.log)

```
mideleg-ssip-route: supervisor software interrupt delegation test
boot: mideleg=0x1444
mideleg: write=0x0 readback=0x1444
mideleg: write=0x2 readback=0x1446
medeleg: write=0x0 readback=0x0
control: s_traps=0 m_traps=0
negctl: msip-readback=1
negctl: s_traps=0 m_traps=0 msip-still-set=1
negctl: msip-after-clear=0
ssi: sip-after-pend=0x2
ssi: spins=0 s_traps=1 scause=0x8000000000000001 sepc=0x8000040e expected=0x8000040e sip-at-entry=0x2
ssi: sip-after-handler=0x0 m_traps=0
quiet: s_traps=1 m_traps=0
final: mideleg-programmed=0x1446
RESULT: PASS
done
```

Runs 2 and 3 are byte-identical to run 1 apart from the harness
timeout line that `timeout(1)` appends after the program parks.
