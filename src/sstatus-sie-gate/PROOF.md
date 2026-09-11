<!-- PROOF-HEADER
Checks: 7
Mismatches: 0
Checksum: 0xf9ecc89d6da3d570
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: sstatus.SIE as the S-mode interrupt gate (backlog item 176)

## What was built

`src/sstatus-sie-gate/`: a bare-metal RISC-V program that verifies
the gate behavior of the `sstatus.SIE` bit on the QEMU `virt` board.
Three files, about 470 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos. Exactly one mechanism is under
test: `sstatus.SIE` gates supervisor-mode interrupt delivery
independently of the pending bits. A pended supervisor timer
interrupt stays pending without trapping while SIE=0, and must trap
the moment SIE=1.

- `ssg_trap.S`: S-mode trap entry. Swaps t0 with sscratch, saves every
  general-purpose register, then calls the C handler
  `ssg_trap_handler()` on a dedicated trap stack. The handler takes
  the one expected supervisor timer trap, records scause/sepc,
  disarms the timer by writing all-ones to `stimecmp` (the source is
  level-triggered, so leaving it armed would re-fire), and counts any
  further trap as unexpected. The M-mode vector `m_trap_entry` is a
  minimal record-and-park handler; no M-mode trap is expected after
  boot, and one would show up in the log as a FAIL rather than a
  silent hang.
- `ssg_main.c`: M-mode boot (probe Sstc via `menvcfg.STCE`, disarm
  both comparators, open one PMP NAPOT entry, grant the
  cycle/time counters via `mcounteren`, delegate the supervisor
  timer interrupt via `mideleg` bit 5, install direct-mode
  `stvec`, `mret` into S-mode) and the S-mode two-phase payload:
  phase A arms `stimecmp` with SIE clear and watches `sip` STIP go
  pending across a 200,000-rdcycle window with zero traps; phase B
  opens SIE exactly inside the labeled wait region (`ssg_loop` /
  `ssg_done`), takes the one trap, and runs a 200,000-rdcycle quiet
  window requiring no re-delivery. A failed check prints `FAIL` and
  flips the verdict; `RESULT: PASS` is printed only when every check
  held. On PASS the machine is shut down via the virt test-device
  finisher so the QEMU process exits 0; on FAIL the hart parks.
- `PROOF.md` (this file), plus an empty `bench-logs/` placeholder
  matching the convention of the other modules.

Build: `make sstatus-sie-gate.elf` (added to `all` in the Makefile).
Run: `timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel sstatus-sie-gate.elf`
(or `make run-sstatus-sie-gate`).

## Configuration under test

- Hart: mhartid = 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the module then drops to S-mode itself.
- Sstc extension present (`menvcfg.STCE` sticks when set, probed at
  boot); `stimecmp` CSR 0x14d is used for the S-mode arm and disarm.
- Only the supervisor timer interrupt is enabled (`sie` bit 5); no
  software, no external interrupts, no PLIC involvement.
- `mideleg = 0x1464` (supervisor timer interrupt bit 5 plus the
  read-only default supervisor-interrupt delegation bits).

## Sequence and controls

1. Phase A, gate closed: `sie.STIE` set, `sstatus.SIE` explicitly
   cleared (read back as 0). `stimecmp = mtime + 5000` (500 us of
   the 10 MHz virtual timebase); the readback is compared against
   the programmed value and only the match outcome is printed.
2. Gated window: 200000 `rdcycle` reads with SIE clear. mtime runs
   past stimecmp inside the window. The trap counter must stay 0
   while `sip` STIP is observed at 1: the interrupt is pending but
   gated. The window is a bounded read count, so it always
   terminates.
3. Phase B, gate open: the single `csrsi sstatus, 2` transition
   happens inside the labeled region between `ssg_loop` and
   `ssg_done`, then a bounded spin waits for the trap. Exactly one
   trap must arrive with `scause == 0x8000000000000005` (interrupt
   bit set, code 5 = supervisor timer interrupt) and `sepc` inside
   the labeled region. The handler's disarm is verified by
   readback: `stimecmp` reads all-ones and `sip` STIP reads 0.
   `sret` restores SIE from SPIE, which the trap entry saved as 1,
   so the gate is open again (read back).
4. Quiet window: 200000 `rdcycle` reads with SIE on and `stimecmp`
   disarmed. The trap counter must stay 1: no re-delivery.

## Results (three QEMU runs)

Three runs were performed after the final build; all print
`RESULT: PASS` with `checks=7 fails=0`, and runs 2 and 3 are fully
byte-identical UART output. The table below is from the final build;
every value is a measured register read or counter, never a
computation from an assumption.

| step | run1 | run2 | run3 |
|---|---|---|---|
| `menvcfg.STCE` probe | present | present | present |
| `stimecmp` at S-mode entry | 0xffffffffffffffff | 0xffffffffffffffff | 0xffffffffffffffff |
| stimecmp readback matches programmed | 1 | 1 | 1 |
| `sstatus.SIE` at phase A (expect 0) | 0 | 0 | 0 |
| gated reads done (expect 200000) | 200000 | 200000 | 200000 |
| `sip` STIP observed in gated window (expect 1) | 1 | 1 | 1 |
| traps during gated window (expect 0) | 0 | 0 | 0 |
| `sstatus.SIE` after release (expect 1) | 1 | 1 | 1 |
| trap scause | 0x8000000000000005 | 0x8000000000000005 | 0x8000000000000005 |
| trap sepc (diagnostic, inside wait loop) | 0x80000388 | 0x80000388 | 0x80000388 |
| traps after gate opened (expect 1) | 1 | 1 | 1 |
| `stimecmp` after handler (expect all-ones) | 0xffffffffffffffff | 0xffffffffffffffff | 0xffffffffffffffff |
| `sip` STIP after handler (expect 0) | 0 | 0 | 0 |
| traps after quiet window (expect 1) | 1 | 1 | 1 |
| checks / mismatches | 7 / 0 | 7 / 0 | 7 / 0 |
| FNV-1a checksum of verdict values | 0xf9ecc89d6da3d570 | 0xf9ecc89d6da3d570 | 0xf9ecc89d6da3d570 |
| RESULT | PASS | PASS | PASS |

What each value means:

- `sip` STIP = 1 with `traps = 0` in the gated window: the Sstc
  comparator went pending (mtime passed stimecmp) and the sip bit
  reflects it, but with `sstatus.SIE` clear the interrupt was not
  taken. Pending state and delivery are independent; the CSR reads
  show it, not an assumption.
- `scause = 0x8000000000000005` on the single trap after SIE is set:
  the interrupt bit (63) set with exception code 5, i.e. a
  supervisor timer interrupt, exactly the delivery the armed
  `stimecmp` is specified to produce, arriving only once the gate
  opened.
- `sepc` inside the labeled wait region: the trap was taken at an
  instruction boundary of the gate-open wait loop, the only place in
  the run where SIE transitions from 0 to 1.
- `stimecmp = 0xffffffffffffffff` and `sip` STIP = 0 after the
  handler: the handler's disarm is confirmed by readback; STIP drops
  because the level-triggered source is gone, and the quiet window
  then shows no re-delivery while SIE stays on.
- `traps = 1` after the quiet window: the pended interrupt produced
  exactly one trap, no more, and no trap ever fired with the gate
  closed.
- The checksum is 64-bit FNV-1a over the deterministic measured
  verdict values (gated trap count, gated STIP observation, total
  trap count, scause, final stimecmp readback, final STIP bit,
  quiet-window trap delta, SIE readback after release). It is
  identical across runs because every measured value is identical.
- The armed `stimecmp` value and `sepc` are excluded from the
  checksum: the armed value depends on boot-time mtime (host
  timing), and `sepc` depends on which wait-loop instruction the
  interrupt lands on (a few instructions of range). Both are printed
  as diagnostics; the verdict lines are byte-identical across runs.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's Sstc, sip, and sstatus CSR model on
  the `virt` machine, not real silicon. The SIE gate behavior is
  architectural, but the observation is against the emulator.
- Only hart 0, only the supervisor timer interrupt, only direct
  S-mode trap entry. External and software interrupts, vectored
  mode, and multi-hart behavior are not tested; the module is
  deliberately that small.
- The gated and quiet windows are 200000 `rdcycle` reads each,
  finite by construction. A leak past the gate or a late
  re-delivery would have shown up as a counter change inside those
  windows.
- An earlier build placed the `csrsi` outside the labeled wait
  region; the trap then landed before the region and the sepc
  bounds check correctly failed the run. The shipped build opens
  the gate inside the region, so the only 0-to-1 SIE transition of
  the run happens there.

## Reproduction

```
make sstatus-sie-gate.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel sstatus-sie-gate.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
