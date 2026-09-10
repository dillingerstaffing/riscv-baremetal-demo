# stvec vectored offsets (backlog item 133)

Installs `stvec` in vectored mode (MODE=1) over a 16-entry table of
single 4-byte `jal` stubs in S-mode on QEMU, delegates the supervisor
timer interrupt (`mideleg` bit 5) and the supervisor external interrupt
(`mideleg` bit 9) to S-mode, then raises both and checks where each
trap lands: the timer interrupt (code 5) must land at BASE + 20 with
`scause = 0x8000000000000005`, the external interrupt (code 9) must
land at BASE + 36 with `scause = 0x8000000000000009`, each exactly
once, with no unexpected traps.

## What it does

1. Boots in M-mode, installs a minimal M-mode trap handler
   (direct-mode `mtvec`, `mscratch` scratch area) that records
   `mcause` and parks: no M-mode trap may fire in this run.
2. Probes Sstc by setting `menvcfg.STCE` and requiring the bit to
   stick; the timer phase arms `stimecmp` directly, so Sstc is
   required.
3. Writes zero to `mideleg` and reads back the forced set (on this
   hart, `0x1444`: the hypervisor extension ORs bits 2, 6, 10, 12
   back in after every write, as in `src/mideleg-route/`), then
   writes bits 5 and 9 and requires the readback to equal the forced
   set with exactly those two bits added (`0x1664`). `medeleg` is
   written zero and read back zero.
4. Parks both timer comparators (`mtimecmp` and `stimecmp` at ~0),
   opens the whole address space to S-mode with one PMP NAPOT R/W/X
   entry, grants S-mode counter access (`mcounteren`), programs the
   PLIC hart-0 S-mode context (context 1: priority 1 for UART source
   10, enable bit 10, threshold 0, every write read back).
5. Writes `stvec` = table address with MODE=1 (readback-verified),
   points `sscratch` at the trap save area, and drops to S-mode via
   `sret`.
6. Phase 1 (timer): S-mode arms `stimecmp` = `time` + 2000000 ticks
   with `sie.STIE` set, then enables `sstatus.SIE`. The stub for
   entry 5 records its own entry address (assembler-resolved `la`,
   not a C label) and the common handler records `scause`,
   disarms `stimecmp` (writes ~0, read back), and raises the done
   flag. The run requires exactly one trap, `scause` the timer
   code, and landing address exactly BASE + 20.
7. Phase 2 (external): one UART byte is looped back into its own
   receiver (same construction as `src/plic/`), the PLIC pending bit
   for source 10 is observed, loopback is switched off, then
   `sie.SEIE` and `sstatus.SIE` are set. The stub for entry 9
   records its entry address; the handler claims source 10 through
   the S-mode context claim register, reads the looped-back byte
   (dropping the level-triggered IRQ line), completes the claim, and
   raises the done flag. The run requires exactly one trap,
   `scause` the external code, landing address exactly BASE + 36,
   and claim id 10.
8. Prints `RESULT: PASS` only if every check held (2 traps total,
   zero unexpected, no M-mode trap during setup); on PASS the
   virt test-device finisher shuts the machine down (QEMU exits 0),
   on FAIL the hart parks instead.

## Files

- `stvv_main.c`: M-mode setup, the S-mode test sequence, UART
  reporting, PASS/FAIL verdict.
- `stvv_trap.S`: the 16-entry vectored table, per-entry stubs, the
  S-mode common handler (records the event log, disarms/claims
  the two interrupt sources), and the M-mode setup-window handler.
- `PROOF.md`: the full proof record with per-run measurements.
- `bench-logs/`: the build log and the three raw QEMU run logs
  (byte-for-byte identical).

## Build and run

From the repo root:

```
make stvec-vectored.elf
make run-stvec-vectored
```

`RESULT: PASS` is printed only when every check holds; the module
then shuts the machine down via the virt test-device finisher, so
the QEMU process exits 0.
