# Proof: M-mode trap handler adds 4 to mepc, faulting load skipped exactly once (backlog item "riscv mepc-resume-skip")

## What was built

`src/mepc-resume-skip/`: a bare-metal RV64 M-mode program that
verifies a trap handler which adds 4 to mepc before `mret` resumes
the hart at the instruction immediately after a faulting instruction,
skipping the fault exactly once. Exactly one mechanism is under test:
the handler's `mepc += 4` resume. Three files, sharing only
`src/boot.S` and `src/uart.c` with the other demos.

- `mrs_main.c`: installs a direct-mode mtvec handler, verifies
  `mie == 0` at boot and clears `mstatus.MIE` so the faulting load is
  the only trap the run can take, then executes a single volatile
  asm block that captures the faulting instruction's address and the
  address of the instruction right after it with in-asm numeric
  local labels (`la reg, 1f` / `la reg, 2f`), loads a sentinel into
  a0 and an unmapped address into a1, executes one `lw a0, 0(a1)`,
  and at label 2 writes a marker word to memory. It prints the
  handler-recorded mcause/mtval/mepc-before/mepc-after and trap
  count, the captured fault/resume addresses, the instruction word
  at the fault address, and the post-resume marker/a0/a1 values,
  and asserts every check listed below. `RESULT: PASS` prints only
  when every check holds. On PASS the machine shuts down through
  the virt test-device finisher (QEMU exits 0); on FAIL the hart
  parks without touching the finisher.
- `mrs_trap.S`: trap entry that swaps t0 with mscratch, records
  mcause, mtval, and the mepc the hardware saved at trap entry
  into `mrs_regs[0..2]`, bumps the trap counter in `mrs_regs[3]`,
  advances mepc by 4, records the adjusted mepc in `mrs_regs[4]`,
  restores t0 via the symmetric second swap, and executes `mret`.
  The handler touches only t0 (scratch-swapped) and t1; a0/a1 are
  untouched so the post-resume code can verify the faulting load
  never committed.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make mepc-resume-skip.elf` (added to `all` in the Makefile).
Run: `timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mepc-resume-skip.elf`
with `~/workspace/qemu/usr/bin/qemu-system-riscv64` (8.2.2) and
`LD_LIBRARY_PATH=~/workspace/qemu/usr/lib/x86_64-linux-gnu:~/workspace/qemu/lib/x86_64-linux-gnu`
(the compat-bin QEMU is broken; missing libfdt/libfuse3).

## Configuration under test

- Hart: hart 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- The faulting load is the only possible trap: `mie` reads `0x0`
  at boot (asserted) and `mstatus.MIE` is cleared explicitly before
  the sequence, so no interrupt source can fire.
- The fault site assembles with `.option norvc`, so the faulting
  instruction is exactly 4 bytes. Verified in the disassembly: the
  faulting `lw a0,0(a1)` sits at `0x80000312` with encoding
  `0x0005a503` (4 bytes), and the first marker instruction (`lui
  t0`) sits at `0x80000316`, exactly 4 bytes later. Without norvc
  the assembler could have picked the 2-byte `c.lw` form for these
  registers, which would have made a fixed +4 skip wrong; the
  program asserts the 4-byte delta from the captured addresses at
  run time rather than trusting the disassembly.
- The faulting instruction's address and the resume address are
  captured with in-asm numeric local labels (`la reg, 1f`,
  `la reg, 2f`), not with C labels-as-values, per the documented
  13.2.0 `&&label` miscompile. The addresses the assertions compare
  against come from the assembler's own label resolution.

## Sequence and controls

1. Setup: mtvec written with the handler address, read back and
   required to be direct mode; mie required 0.
2. The single asm block runs: `la` captures `fault` (label 1) and
   `resume` (label 2); a0 gets the sentinel
   `0xa0a0a0a0a0a0a0a0`; a1 gets the unmapped address
   `0xffffffffc0000000` (`li a1, 0xC0000000` sign-extends on RV64);
   the `lw a0, 0(a1)` traps with a load access fault; the handler
   records mcause/mtval/mepc-at-entry, bumps the counter, records
   mepc+4, and mrets; execution resumes at label 2, which writes
   `0xdeadbeefdeadbeef` to the marker slot, then reads a0 and a1
   back to C.
3. Checks: trap count exactly 1; mcause == 5 (load access fault);
   mtval == 0xffffffffc0000000; mepc-at-entry == captured fault
   address; mepc-after - mepc-before == 4; mepc-after == captured
   resume address; resume - fault == 4; the 32-bit word at the
   fault address == 0x0005a503 (`lw a0, 0(a1)`); the marker slot
   holds the marker (the resume instruction ran); a0 still holds
   its sentinel (the faulting load never committed); a1 still
   holds the unmapped address (the handler did not clobber it).

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

The three run logs are byte-identical; the table shows run1, and
runs 2 and 3 matched every value.

| step | run1 | run2 | run3 |
|---|---|---|---|
| mtvec (handler installed, direct) | 0x800001c8 | 0x800001c8 | 0x800001c8 |
| mie at boot (expect 0x0) | 0x0 | 0x0 | 0x0 |
| captured fault addr | 0x80000312 | 0x80000312 | 0x80000312 |
| captured resume addr | 0x80000316 | 0x80000316 | 0x80000316 |
| resume - fault (expect 4) | 4 | 4 | 4 |
| trap count (expect 1) | 1 | 1 | 1 |
| mcause (expect 5, load access fault) | 0x5 | 0x5 | 0x5 |
| mtval (expect 0xffffffffc0000000) | 0xffffffffc0000000 | same | same |
| mepc before (expect fault addr) | 0x80000312 | 0x80000312 | 0x80000312 |
| mepc after (expect resume addr) | 0x80000316 | 0x80000316 | 0x80000316 |
| mepc delta (expect 4) | 4 | 4 | 4 |
| insn at fault addr (expect 0x5a503) | 0x5a503 | 0x5a503 | 0x5a503 |
| marker (expect 0xdeadbeefdeadbeef) | 0xdeadbeefdeadbeef | same | same |
| a0 after (expect sentinel 0xa0...) | 0xa0a0a0a0a0a0a0a0 | same | same |
| a1 after (expect 0xffffffffc0000000) | 0xffffffffc0000000 | same | same |
| QEMU exit code | 0 | 0 | 0 |
| RESULT | PASS | PASS | PASS |

What each value means:

- `mcause = 0x5` (5): load access fault, the correct code for a
  load from an unmapped address on this machine. `mtval =
  0xffffffffc0000000` is the faulting virtual address, exactly the
  sign-extended value the `li` put into a1 (a1 reads back the same
  value after resume, confirming the handler did not disturb it).
- `mepc before = 0x80000312` equals the captured fault address
  from the assembler's label, read from the CSR by the handler,
  not inferred. `insn = 0x5a503` confirms the trapping instruction
  is the intended `lw a0, 0(a1)` (matches the disassembly
  `0x0005a503`).
- `mepc after = 0x80000316` equals the captured resume address,
  and the delta is exactly 4 in both the handler's records and the
  independent `la` captures: two separate measurements of the
  same +4 advance agree.
- `marker = 0xdeadbeefdeadbeef`: the store at label 2 ran, so the
  hart resumed at faulting+4 and executed the very next
  instruction. A +8 advance would have skipped the marker; a
  +0 advance would have re-trapped.
- Trap count exactly 1 with mie == 0 and MIE clear: the fault was
  taken once and never re-delivered. Had the handler resumed at
  the faulting instruction, the second trap would have made the
  count 2 and failed the run.
- `a0 = 0xa0a0a0a0a0a0a0a0` after resume: the faulting load never
  committed its destination register. QEMU did not write a0 on the
  faulting access, and the handler left a0 alone, so the sentinel
  surviving is direct evidence the fault was skipped, not executed.
- QEMU exit code 0 on all runs: the finisher shutdown path
  executed, i.e. `RESULT: PASS` with no parked FAIL.

## Toolchain note (measured, not assumed)

Built with the distro `riscv64-unknown-elf-gcc` 13.2.0 via the
repo Makefile flags (`-Wall -Wextra -O2 -march=rv64imac_zicsr
-mabi=lp64 -mcmodel=medany`). The build log in
`bench-logs/build.log` records the exact commands; zero warnings,
only the linker's benign RWX-LOAD-segment warning also seen on
the sibling builds. No labels-as-values are used anywhere in the
module, per the documented 13.2.0 `&&label` miscompile: the
fault/resume addresses are taken with in-asm numeric local
labels (`la reg, 1f` / `la reg, 2f`), which the assembler
resolves exactly, and the handler computes the resume as
`mepc + 4` in the trap entry itself.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's trap and CSR models on the `virt`
  machine, not real silicon. The `mepc += 4` skip semantics are
  architectural (mepc holds the faulting instruction's address for
  synchronous traps, and `mret` resumes at mepc), but the fault
  cause code for this address (5, load access fault) and the
  unmapped address itself are emulator- and machine-specific.
- Only hart 0, only M-mode, only the synchronous load-fault path.
  Store faults, misaligned faults, interrupt-driven entry, S-mode
  delegation, vectored mtvec, and multi-hart behavior are not
  tested here; the module is deliberately that small.
- The +4 skip is correct only because the faulting instruction is
  exactly 4 bytes; the program asserts the 4-byte delta at run
  time, but a 2-byte compressed faulting instruction would need a
  different advance, which this module does not attempt.
- The three runs are byte-identical; no host-varying values appear
  in the check path.

## Reproduction

```
make mepc-resume-skip.elf
export LD_LIBRARY_PATH="$HOME/workspace/qemu/usr/lib/x86_64-linux-gnu:$HOME/workspace/qemu/lib/x86_64-linux-gnu"
timeout 30 ~/workspace/qemu/usr/bin/qemu-system-riscv64 -machine virt -nographic -bios none -kernel mepc-resume-skip.elf
```

Linked flat at 0x80000000 via `link.ld`. Build log:
`bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`.
Each ends with `RESULT: PASS (traps=1)` and the finisher shutdown
(exit 0).
