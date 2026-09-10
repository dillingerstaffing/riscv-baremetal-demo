<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF.md: mtvec vectored trap dispatch (backlog item 74)

## What was built

A bare-metal RV64 module that installs `mtvec` in vectored mode
(MODE=1) over a 16-entry table of single 32-bit `jal` stubs, then
provokes two real traps and checks, from the address of the stub that
actually ran, that each trap landed where vectored dispatch says it
should.

Files: `mtv_trap.S`, `mtv_main.c`. Raw UART logs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(byte-for-byte identical across three boots).

## Measured results (QEMU 8.2.2, `virt`, all three runs)

From `run1.log` (identical in run2/run3):

```
mtvec: written=0x80000201 readback=0x80000201
table: base=0x80000200 entry7=0x8000021c (expect 0x8000021c) entry8=0x80000220 (expect 0x80000220)
control: traps-before-ecall=0 (expect 0)
ecall: count=2 mcause1=0x8 landing1=0x80000200 mcause2=0x8 landing2=0x80000200
timer: count=1 mcause=0x8000000000000007 landing=0x8000021c (expect 0x8000021c)
timer: mtimecmp-after-handler=0xffffffffffffffff (expect 0xffffffffffffffff)
timer: count-after-quiet=1 (expect 1)
RESULT: PASS
```

Decoded:

- `mtvec` = `0x80000201`: BASE `0x80000200`, MODE 1 (vectored).
  Readback equals the written value.
- Vector table base `0x80000200`, 256-byte aligned. Entry 7 at
  `0x8000021c` = BASE + 28. Entry 8 at `0x80000220` = BASE + 32.
  Objdump confirms all 16 entries are one 32-bit `jal`, 4 bytes apart.
- U-mode ecall (exception code 8): two traps, both with
  `mcause = 0x8`, both landing at `0x80000200` (BASE, entry 0's stub).
- Machine timer interrupt (code 7): one trap,
  `mcause = 0x8000000000000007`, landing at `0x8000021c` (BASE + 28,
  entry 7's stub).
- After the timer trap, the handler disarmed the source:
  `mtimecmp` reads `0xffffffffffffffff`, and a further quiet window
  produced no second delivery (`count-after-quiet=1`).
- A control window before any trigger produced zero traps.

## Why the synchronous ecall lands at BASE, not BASE + 32

The backlog item asked for an "M-mode ecall, exception code 8" at
BASE + 32. Two facts make that impossible as written, both measured:

1. An `ecall` executed in M-mode raises exception code 11, not 8.
   Only a U-mode `ecall` yields `mcause = 8`, so the module drops to
   U-mode (with a PMP NAPOT R/W/X entry, as in `src/umode/`) to
   produce the code-8 trap.
2. On QEMU 8.2.2, vectored `mtvec` (MODE=1) offsets only
   *asynchronous* traps: the trap entry computes
   `BASE + 4*cause` for interrupts and plain `BASE` for synchronous
   exceptions (confirmed in QEMU's `cpu_helper.c` trap delivery and
   by the measured landing addresses). The two genuine U-mode ecalls
   both entered at `0x80000200` while `mcause` read 8.

So the module asserts the observed rule: interrupts vector to
BASE + 4*cause (timer, code 7 -> `0x8000021c`), synchronous
exceptions enter at BASE (ecall, code 8 -> `0x80000200`). Fabricating
a BASE + 32 landing for the synchronous trap would have meant lying
about what the machine did.

## A real bug found and fixed during bring-up

After the U-mode excursion, the timer interrupt never fired
(`mip` stayed 0 even with `mtime` past `mtimecmp`). Bisecting with
minimal experiments showed the cause was in the module, not QEMU:
the `mret` that drops to U-mode cleared `mstatus.MIE`, because
`MPIE` was 0 at that point and `mret` restores `MIE` from `MPIE`.
Every later `mret` then kept `MIE=0`, so no interrupt could ever be
taken (a software-interrupt probe via `msip` failed the same way).

Fix: `mtv_umode_entry` sets `MPIE` before the dropping `mret`, so
the return to M-mode restores `MIE=1`. After the fix, the timer
fires exactly once at BASE + 28 and the run reports PASS.

## Limits

- Emulator, not silicon: all behavior above was measured on QEMU
  8.2.2 (`virt` machine). The sync/async vectoring split is this
  QEMU's behavior; the RISC-V privileged spec vectors synchronous
  exceptions in MODE=1 too, so real hardware may differ.
- CSR encodings (`mcause` 8 and `0x8000000000000007`, `mtvec` MODE=1)
  are architectural; the linked addresses (`0x80000200`,
  `0x8000021c`, `0x80000220`) are specific to this image and run.
- Toolchain: `riscv64-unknown-elf-gcc` 13.2.0, `-O2`.
