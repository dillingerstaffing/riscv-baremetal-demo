<!-- PROOF-HEADER
Checks: 14
Mismatches: 0
Checksum: 0xddb82db8ae1a497d
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mstatus.MIE global interrupt-enable gate (backlog item 156)

## What was built

`src/mie-toggle/`: a bare-metal RISC-V program that verifies the
gate behavior of the `mstatus.MIE` bit on the QEMU `virt` board. Two
files, about 330 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos. Exactly one mechanism is under
test: a pending interrupt whose enable bit is set in `mie` does not
reach the trap handler while `mstatus.MIE` is clear, and reaches it
exactly once the moment MIE is set.

- `mie-toggle_trap.S`: M-mode trap entry. Saves t0 via mscratch,
  records mcause/mepc, then calls the C handler
  `mie_c_handle()`, which disarms the CLINT timer first
  (mtimecmp = all-ones: the source is level-triggered, so leaving it
  armed would re-fire the instant `mret` restores MIE), bumps the
  trap counter, and records mcause/mepc. All interrupted state is
  restored before `mret`.
- `mie-toggle_main.c`: installs direct-mode `mtvec`, enables only
  `mie.MTIE` with `mstatus.MIE` clear (both read back), arms the
  CLINT timer, observes the pending-but-gated window, opens the
  gate, and verifies the single trap and its disarm, with a check on
  every step. A failed check prints `FAIL` and flips the verdict;
  `RESULT: PASS` is printed only when every check held.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make mie-toggle.elf` (added to `all` in the Makefile).
Run: `timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mie-toggle.elf`
(or `make run-mie-toggle`).

## Configuration under test

- Hart: mhartid = 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- CLINT mtime at `0x0200bff8`, mtimecmp for hart 0 at `0x02004000`,
  both accessed 64-bit (matching how `src/preempt/clint.c` drives
  them).
- Only the machine timer interrupt is enabled (`mie` bit 7); no
  software, no external interrupts, no PLIC involvement.

## Sequence and controls

1. Arm: `mie.MTIE` set, `mstatus.MIE` cleared (both read back), then
   `mtimecmp = mtime + 5000` (500 us of the 10 MHz virtual timebase).
   The mtimecmp readback is compared against the programmed value.
2. Gated window: spin for 100000 mtime ticks (10 ms) with MIE still
   clear. mtime runs well past mtimecmp during this window. The trap
   counter must stay 0 and `mip.MTIP` must read 1: pending, but
   gated. The window is bounded by mtime ticks, not by instruction
   spins, so it always terminates and always outlasts the arm point.
3. Release: `csrsi mstatus, 8`; read back that MIE is 1. Wait for
   the trap with a large spin budget. Exactly one trap must arrive
   with `mcause == 0x8000000000000007` (interrupt bit set, code 7 =
   machine timer interrupt). The handler's disarm is verified by
   readback: mtimecmp reads all-ones and `mip.MTIP` reads 0.
4. Quiet window: another 100000 mtime ticks with MIE still set. The
   trap counter must stay 1 and `mip.MTIP` must stay 0: no
   re-delivery.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

All three runs' UART output is byte-identical (same md5), including
the checksum line. The only per-run difference in the raw logs is the
trailing `terminating on signal 15` line, which is the `timeout`
wrapper killing QEMU after the program prints `done`, same as the
other modules' logs.

| step | run1 | run2 | run3 |
|---|---|---|---|
| mtvec (direct mode) | 0x800001c8 | 0x800001c8 | 0x800001c8 |
| `mie.MTIE` / `mstatus.MIE` at arm | 1 / 0 | 1 / 0 | 1 / 0 |
| mtimecmp readback matches programmed | 1 | 1 | 1 |
| traps during gated window (expect 0) | 0 | 0 | 0 |
| `mip.MTIP` during gated window (expect 1) | 1 | 1 | 1 |
| `mstatus.MIE` after release (expect 1) | 1 | 1 | 1 |
| trap mcause | 0x8000000000000007 | 0x8000000000000007 | 0x8000000000000007 |
| traps after release (expect 1) | 1 | 1 | 1 |
| mtimecmp after handler (expect all-ones) | 0xffffffffffffffff | 0xffffffffffffffff | 0xffffffffffffffff |
| `mip.MTIP` after handler (expect 0) | 0 | 0 | 0 |
| traps after quiet window (expect 1) | 1 | 1 | 1 |
| `mip.MTIP` after quiet window (expect 0) | 0 | 0 | 0 |
| checks / mismatches | 14 / 0 | 14 / 0 | 14 / 0 |
| FNV-1a checksum of verdict values | 0xddb82db8ae1a497d | 0xddb82db8ae1a497d | 0xddb82db8ae1a497d |
| RESULT | PASS | PASS | PASS |

What each value means:

- `mip.MTIP = 1` with `traps = 0` in the gated window: the CLINT
  comparator went pending (mtime passed mtimecmp) and the mip bit
  reflects it, but with `mstatus.MIE` clear the interrupt was not
  taken. This is the gate behavior under test, read from the CSR,
  not inferred.
- `mcause = 0x8000000000000007` on the single trap: the interrupt
  bit (63) set with exception code 7, i.e. a machine timer
  interrupt, exactly the delivery the armed CLINT timer is specified
  to produce.
- `mtimecmp = 0xffffffffffffffff` and `mip.MTIP = 0` after the
  handler: the handler's disarm is confirmed by readback; MTIP drops
  because the level-triggered source is gone, and the quiet window
  then shows no re-delivery.
- `traps = 1` after the quiet window: the pending interrupt produced
  exactly one trap, no more.
- The checksum is 64-bit FNV-1a over the ten measured verdict
  values (mtip_gated, traps_gated, mcause, traps_after,
  mtimecmp_after, mtip_after, traps_quiet, mtip_quiet, mie_after,
  mtie_after). It is identical across runs because every measured
  value is identical.
- The programmed mtimecmp value itself is not printed: it depends
  on the boot-time mtime, which varies by host timing. Only the
  readback-match outcome (deterministic) is printed.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's CLINT and CSR model on the `virt`
  machine, not real silicon. The MIE gate behavior is architectural,
  but the observation is against the emulator.
- Only hart 0, only M-mode, only the machine timer interrupt. S-mode
  delegation, the software and external interrupt sources, and
  multi-hart behavior are not tested; the module is deliberately that
  small.
- The gated and quiet windows are 100000 mtime ticks (10 ms of
  virtual time) each, finite by construction. A leak past the gate or
  a late re-delivery would have shown up as a counter change inside
  those windows.

## Reproduction

```
make mie-toggle.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mie-toggle.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; QEMU is terminated by `timeout` afterwards
because the bare-metal image never exits QEMU on its own).
