<!-- PROOF-HEADER
Checks: 15
Mismatches: 0
Checksum: n/a
Throughput: n/a
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mideleg selective-routing test (backlog item 153)

## What was built

`src/mideleg-route/`: a bare-metal RISC-V program that sets `mideleg`
to delegate only the supervisor external interrupt (bit 9,
readback-verified), then fires a supervisor timer interrupt and a
PLIC supervisor external interrupt and checks where each lands. Four
files, sharing only `src/boot.S` and `src/uart.c` with the other
demos. Exactly one mechanism is under test: selective interrupt
routing through `mideleg`.

- `midr_trap.S`: M-mode trap entry (records `mcause`/`mepc` in
  `m_regs`, clears a pended `STIP` bit so the timer interrupt fires
  exactly once; an S-mode `ecall` is serviced as a `mideleg`
  readback request into `m_regs[5]`, since S-mode cannot read that
  CSR itself) and S-mode trap entry (records `scause`/`sepc` in
  `s_regs`, claims the interrupt through the PLIC S-mode-context
  claim register, reads the looped-back UART byte to drop the IRQ
  line, completes the claim, raises the done flag).
- `midr_main.c`: installs direct-mode `mtvec`/`mscratch`, records
  boot delegation values, programs the selective delegation with
  readback checks, runs phase (a) the M-mode timer-interrupt trap,
  programs the PLIC S-mode context with readbacks, asserts the UART
  interrupt in loopback, drops to S-mode for phase (b), checks the
  S-mode trap registers, re-reads `mideleg` via the M-mode `ecall`
  service, and prints `RESULT: PASS` only when all 15 checks held.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make mideleg-route.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel mideleg-route.elf`
(or `make run-mideleg-route`).

Toolchain: xpack riscv-none-elf-gcc 15.2.0 (via
`~/workspace/toolchains/compat-bin`), QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`.
- The program is the only code running. Phase (a) runs in M-mode;
  phase (b) runs in S-mode after a `sret` drop (one PMP NAPOT entry
  opens the whole address space to S-mode; `stvec`/`sscratch`
  installed beforehand). `medeleg` is zero throughout, so no
  exception is ever delegated.
- No CLINT timer is involved: the supervisor timer interrupt is
  pended directly by writing `STIP` in `mip`, exactly as the backlog
  item prescribes.

## Sequence and controls

1. Record boot-time `medeleg` (`0x0`) and `mideleg` (`0x1444`).
2. Write `0` to `mideleg`, read back: exposes the forced set
   (`0x1444`). Write `0x200`, read back: must equal the forced set
   with only bit 9 added (`0x1644`); bit 5 must read back clear.
   Write `0` to `medeleg`, read back `0x0`.
3. Phase (a): set `mie.STIE` and `mstatus.MIE`, pend `STIP` in
   `mip`. The M-mode handler must record `mcause =
   0x8000000000000005` exactly once and clear `STIP`.
4. Program the PLIC hart-0 S-mode context: `priority[10] = 1`,
   context-1 enable bit 10, context-1 threshold 0; each write read
   back.
5. Assert the UART interrupt in internal loopback (one transmitted
   byte, `IER_RDI` on); the PLIC pending bit 10 must read 1 before
   loopback is switched off.
6. Drop to S-mode, set `sie.SEIE`, then set `sstatus.SIE`. The
   pending interrupt is taken at the next instruction boundary.
7. In S-mode: require exactly one trap, `scause =
   0x8000000000000009`, `sepc` equal to the address of the
   interrupted instruction (the `nop` right after the `SIE` enable,
   captured with an in-assembly local label), and claim id 10.
8. Re-read `mideleg` through the M-mode `ecall` service; it must
   equal the programmed readback. Print `RESULT: PASS` only if all
   15 checks held.

## Measured results

All quantities below are identical across the three runs
(byte-identical logs modulo the harness timeout line):

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| boot `medeleg` / `mideleg` | `0x0` / `0x1444` | identical | identical |
| `mideleg` write `0x0` readback | `0x1444` | identical | identical |
| `mideleg` write `0x200` readback | `0x1644` | identical | identical |
| `medeleg` write `0x0` readback | `0x0` | identical | identical |
| timer: M-mode traps / `mcause` | 1 / `0x8000000000000005` | identical | identical |
| PLIC `priority[10]` / ctx1 `enable10` / ctx1 `thresh` | 1 / 1 / 0 | identical | identical |
| assert: `pending10` (spins) | 1 (0) | identical | identical |
| ext: S-mode traps / `scause` | 1 / `0x8000000000000009` | identical | identical |
| ext: `sepc` / interrupted-instruction address | `0x800002e0` / `0x800002e0` | identical | identical |
| ext: S-mode claim id | 10 | identical | identical |
| final `mideleg` (via M-mode ecall) | `0x1644` | identical | identical |
| verdict | PASS | PASS | PASS |

The 15 checks: (1) `mideleg` write changed only bit 9,
(2) `mideleg` bit 5 clear, (3) `medeleg` readback zero,
(4) timer trap count 1, (5) timer `mcause`,
(6-8) PLIC priority/enable/threshold readbacks,
(9-10) UART IRQ pending observed, (11) S-mode trap count 1,
(12) `scause`, (13) `sepc` equals the interrupted-instruction
address, (14) claim id, (15) final `mideleg` unchanged. Zero
failures on all three runs.

## The mideleg readback

Writing `0x200` to `mideleg` reads back `0x1644`, not `0x200`.
This is not a failed write: QEMU 8.2.2's `rmw_mideleg64`
(`target/riscv/csr.c`) computes
`mideleg = (old & ~mask) | (new & mask)` with
`mask = delegable_ints`, then ORs `HS_MODE_INTERRUPTS` back in
whenever the hypervisor extension is present. On this hart the
hypervisor extension is present, so bits 2, 6, 10, 12
(`VSSIP`/`VSTIP`/`VSEIP`/`SGEIP`) are forced to 1 on every write
and read back 1 regardless of the written value. The observed
arithmetic matches that code path exactly: boot `0x1444`, write
`0x0` reads back `0x1444`, write `0x200` reads back
`0x200 | 0x1444 = 0x1644`. The program's check therefore verifies
the property the backlog item actually needs: among the
software-writable bits, the write set only bit 9 (readback equals
the zero-write readback with bit 9 added), and bit 5 reads back
clear.

## Raw QEMU output

### Run 1 (bench-logs/midr-run1.log)

```
mideleg-route: selective delegation routing test
boot: medeleg=0x0 mideleg=0x1444
mideleg: write=0x0 readback=0x1444
mideleg: write=0x200 readback=0x1644
medeleg: write=0x0 readback=0x0
timer: m_traps=1 mcause=0x8000000000000005
plic: priority[10]=1 enable10=1 thresh=0
assert: pending10=1 (spins=0)
ext: s_traps=1 scause=0x8000000000000009 sepc=0x800002e0 expected=0x800002e0 claim=10
final: mideleg=0x1644
RESULT: PASS
done
```

### Run 2 (bench-logs/midr-run2.log)

```
mideleg-route: selective delegation routing test
boot: medeleg=0x0 mideleg=0x1444
mideleg: write=0x0 readback=0x1444
mideleg: write=0x200 readback=0x1644
medeleg: write=0x0 readback=0x0
timer: m_traps=1 mcause=0x8000000000000005
plic: priority[10]=1 enable10=1 thresh=0
assert: pending10=1 (spins=0)
ext: s_traps=1 scause=0x8000000000000009 sepc=0x800002e0 expected=0x800002e0 claim=10
final: mideleg=0x1644
RESULT: PASS
done
```

### Run 3 (bench-logs/midr-run3.log)

```
mideleg-route: selective delegation routing test
boot: medeleg=0x0 mideleg=0x1444
mideleg: write=0x0 readback=0x1444
mideleg: write=0x200 readback=0x1644
medeleg: write=0x0 readback=0x0
timer: m_traps=1 mcause=0x8000000000000005
plic: priority[10]=1 enable10=1 thresh=0
assert: pending10=1 (spins=0)
ext: s_traps=1 scause=0x8000000000000009 sepc=0x800002e0 expected=0x800002e0 claim=10
final: mideleg=0x1644
RESULT: PASS
done
```

(Each log ends with the harness `timeout` killing the hart parked
in its final `wfi` loop; exit status 124 is the harness, not a
failure. The `done` line is the program's last output.)

## Limits

- Emulator behavior, not silicon: everything above was measured on
  QEMU 8.2.2's `virt` machine. The forced `mideleg` bits and the
  exact trap-timing behavior are properties of this QEMU build.
- The S-mode external-interrupt path depends on the PLIC's
  supervisor context and on asserting the UART interrupt through
  internal loopback (the same construction as `src/plic/`); the
  claim/complete round trip itself is not re-verified here beyond
  the claim id readback of 10.
- `sepc` equaling the interrupted-instruction address relies on the
  interrupt being taken at the first instruction boundary after
  `sstatus.SIE` is set; observed on all three runs on this QEMU
  build.
- The final `mideleg` readback goes through an M-mode `ecall`
  service because S-mode cannot read `mideleg` directly; this
  assumes M-mode ecalls are not delegated (`medeleg = 0`, as
  programmed).
- Single hart; no concurrency or multi-hart interrupt routing is
  exercised.
