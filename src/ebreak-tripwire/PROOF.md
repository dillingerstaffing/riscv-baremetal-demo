<!-- PROOF-HEADER
Checks: 13
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: a planted ebreak traps with mcause 3 and mepc naming the tripwire (both encodings)

## What was built

`src/ebreak-tripwire/`: a bare-metal RISC-V program that verifies, on
the QEMU `virt` board, the exact trap contract of the breakpoint
instruction in both of its encodings: the 4-byte `ebreak`
(`0x00100073`, assembled under `.option norvc`) and the 2-byte
compressed `c.ebreak` (`0x9002`, the form `__builtin_trap()` emits at
`-O2`, confirmed by objdump below). For each tripwire the program
captures the tripwire address and the post-tripwire address with
in-asm numeric local labels, stores the resume address into the save
area before the tripwire, executes the single instruction, and lets an
M-mode trap handler record mcause and the entry mepc, then resume at
the stored address. The module shares only `src/boot.S` and the UART
driver with the other demos.

- `ebt_main.c`: M-mode setup (direct-mode `mtvec`, `mscratch` on the
  20-word save area, `mstatus.MIE` cleared and `mie` read back as 0 so
  the two tripwires are the only traps the run can take), then phase A
  (4-byte `ebreak`) and phase B (2-byte `c.ebreak`), each in one
  volatile asm block holding the address captures, the resume-address
  store, the tripwire, and a post-tripwire marker. Thirteen in-program
  checks, then the UART report and `RESULT: PASS/FAIL`.
- `ebt_trap.S`: M-mode trap entry. Selects the record for the current
  trap (trap count, 3 words each: mcause, entry mepc, resume address),
  records mcause and mepc, loads mepc from the stored resume address,
  bumps the counter, restores the parked temporaries, `mret`. Touches
  only t0/t1/t2, all saved and restored.
- Exit path: on PASS the virt test-device finisher word `0x5555` at
  `0x100000` shuts the machine down (QEMU exits 0); on FAIL the hart
  parks in `wfi` without touching the finisher, so under `timeout` a
  FAIL is observable as exit 124 as well as the `RESULT: FAIL` line.

Build integration: `Makefile` gains `ebreak-tripwire.elf`,
`run-ebreak-tripwire`, and the module is in `all` and `clean`.

## Configuration under test

- Hart 0, M-mode, `virt` machine, `-bios none`, flat-linked at
  0x80000000 via `link.ld`.
- Direct-mode `mtvec` at the module handler; `mie == 0` and
  `mstatus.MIE` clear, so no interrupt can masquerade as a tripwire.
- Phase A tripwire assembled under `.option norvc` (exactly the
  4-byte `ebreak`); phase B under `.option rvc` (exactly the 2-byte
  `c.ebreak`).

## Sequence and controls

1. Install handler, verify `mtvec`/`mie`, clear MIE.
2. Phase A: capture tripwire/post addresses, store resume, execute
   `ebreak`, write marker A after it.
3. Phase B: capture tripwire/post addresses, store resume, execute
   `c.ebreak`, write marker B after it.
4. Read both trap records; check mcause == 3, mepc == tripwire
   address, the encoding at mepc (`0x00100073` / `0x9002`), resume
   delta (4 / 2 bytes), stored resume == post address, both markers
   present, trap count == 2.
5. Print the report, verdict, finisher shutdown or wfi park.

The compiler cannot reorder or delete anything inside either tripwire
block: each is a single `volatile` asm block, and the checked values
travel to C only through the block's output operands and the
handler's records. No labels-as-values are used anywhere, per the
documented 13.2.0 `&&label` concern; the tripwire/post addresses are
taken with in-asm numeric local labels (`la reg, 1f` / `la reg, 2f`),
which the assembler resolves exactly.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

| value | run1 | run2 | run3 |
|---|---|---|---|
| mtvec (handler installed, direct) | 0x800001c8 | 0x800001c8 | 0x800001c8 |
| mie at boot (expect 0x0) | 0x0 | 0x0 | 0x0 |
| trap count (expect 2) | 2 | 2 | 2 |
| phase A: tripwire addr | 0x800002fe | 0x800002fe | 0x800002fe |
| phase A: post addr | 0x80000302 | 0x80000302 | 0x80000302 |
| phase A: post - tripwire (expect 4) | 4 | 4 | 4 |
| phase A trap: mcause (expect 0x3) | 0x3 | 0x3 | 0x3 |
| phase A trap: mepc (expect tripwire addr) | 0x800002fe | 0x800002fe | 0x800002fe |
| phase A: insn at mepc (expect 0x100073) | 0x100073 | 0x100073 | 0x100073 |
| phase A: marker (expect 0xebbe4a) | 0xebbe4a | 0xebbe4a | 0xebbe4a |
| phase B: tripwire addr | 0x80000320 | 0x80000320 | 0x80000320 |
| phase B: post addr | 0x80000322 | 0x80000322 | 0x80000322 |
| phase B: post - tripwire (expect 2) | 2 | 2 | 2 |
| phase B trap: mcause (expect 0x3) | 0x3 | 0x3 | 0x3 |
| phase B trap: mepc (expect tripwire addr) | 0x80000320 | 0x80000320 | 0x80000320 |
| phase B: halfword at mepc (expect 0x9002) | 0x9002 | 0x9002 | 0x9002 |
| phase B: marker (expect 0xebbe4b) | 0xebbe4b | 0xebbe4b | 0xebbe4b |
| QEMU exit code | 0 | 0 | 0 |
| RESULT | PASS | PASS | PASS |

All three runs are value-identical. The shipped binary's
disassembly (`bench-logs/tripwire-dis.txt`) shows the emitted
instructions verbatim: `00100073 ebreak` at `0x800002fe` with the
next instruction at `0x80000302`, and `9002 ebreak` (the compressed
form, printed by objdump as `ebreak`) at `0x80000320` with the next
instruction at `0x80000322`.

What each value means:

- `mcause = 0x3` for both tripwires is the architecturally defined
  breakpoint code; both traps came from the intended instructions,
  confirmed by the encoding readbacks and by `mepc` equaling the
  assembler-captured tripwire address in both cases. The hardware
  names the exact PC of the planted instruction: that is the whole
  point of the tripwire.
- `insn = 0x100073` at the phase A mepc and `0x9002` at the phase B
  mepc: the two legal spellings of the breakpoint, 4 bytes and
  2 bytes. A handler that resumes past the tripwire must skip the
  right length for the form it planted; the delta checks (4 and 2)
  prove which form was executed.
- Both markers present with trap count exactly 2 and mie == 0:
  each tripwire was taken once and resumed past exactly once; a
  re-executed tripwire would have made the count 3 and failed the
  run, and no interrupt fired.
- QEMU exit code 0 on all runs: the finisher shutdown path
  executed, i.e. `RESULT: PASS` with no parked FAIL.

## Toolchain note (measured, not assumed)

Built with `riscv64-unknown-elf-gcc` 13.2.0 (Ubuntu
13.2.0-11ubuntu1) with the repo Makefile flags (`-Wall -Wextra -O2
-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`). The build log in
`bench-logs/build.log` records the exact commands; zero warnings,
only the linker's benign RWX-LOAD-segment warning also seen on the
sibling builds.

Separately, `__builtin_trap()` was compiled alone with the same
flags and disassembled (`bench-logs/builtin-trap-dis.txt`): at -O2
it emits the 2-byte compressed `c.ebreak` (`9002`), placed by the
compiler in `.text.unlikely`. So the C spelling of "halt here" and
the hand-written 4-byte `ebreak` are the same trap in two encodings,
and a resume-past handler must skip 2 bytes for the compiler's form.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's trap model on the `virt` machine, not
  real silicon. The breakpoint trap code (mcause 3) and the mepc
  contract are architectural; the addresses and the UART log are
  machine-specific.
- Only hart 0, only M-mode, only the two breakpoint encodings, with
  a direct-mode handler and interrupts off. S-mode delegation,
  vectored mtvec, debugger-driven breakpoints, and multi-hart
  behavior are not tested here; the module is deliberately that
  small.
- The three runs are value-identical; no host-varying values appear
  in the check path.

## Reproduction

```
make ebreak-tripwire.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel ebreak-tripwire.elf
```

Linked flat at 0x80000000 via `link.ld`. Build log:
`bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`.
Disassembly excerpts: `bench-logs/tripwire-dis.txt` (both tripwire
encodings in the shipped binary),
`bench-logs/builtin-trap-dis.txt` (`__builtin_trap()` emitting
`c.ebreak`). Each run ends with `RESULT: PASS (traps=2)` and the
finisher shutdown (exit 0).
