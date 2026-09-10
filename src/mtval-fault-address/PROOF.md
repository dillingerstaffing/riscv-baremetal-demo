<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: load and store access faults report the same exact faulting address in mtval (backlog item 134, "riscv mtval-fault-address")

## What was built

`src/mtval-fault-address/`: a bare-metal RV64 M-mode program that
answers one question: when the same unmapped address faults once on
a load and once on a store, does mtval report the exact same faulting
address for both traps (mcause 0x5 vs 0x7)? It issues one
`lw a0, 0(a1)` and one `sw a0, 0(a1)` at the same unmapped address,
publishes the mcause/mepc/mtval triple for each trap, and asserts the
two mtval values are equal and equal to the faulting address. Three
files, sharing only `src/boot.S` and `src/uart.c` with the other
demos.

- `mfa_main.c`: installs a direct-mode mtvec handler, verifies
  `mie == 0` at boot and clears `mstatus.MIE` so the two faulting
  accesses are the only traps the run can take, then runs two
  volatile asm blocks. Each block captures the faulting instruction's
  address and the resume address with in-asm numeric local labels
  (`la reg, 1f` / `la reg, 2f`), stores the resume address into its
  record in the save area before the fault, loads a sentinel into a0
  and the unmapped address into a1, and executes the single faulting
  access. It prints both mcause/mepc/mtval triples, the captured
  addresses, and the mtval equality verdict, and asserts every check
  listed below. `RESULT: PASS` prints only when every check holds.
  On PASS the machine shuts down through the virt test-device
  finisher (QEMU exits 0); on FAIL the hart parks without touching
  the finisher.
- `mfa_trap.S`: trap entry that swaps t0 with mscratch, parks t1/t2
  in spare slots, reads the trap counter, records mcause, mtval, and
  the mepc the hardware saved at trap entry into that trap's 4-word
  record (record 0 for the load fault, record 1 for the store fault),
  loads mepc from the resume address the test stored, bumps the trap
  counter, restores t2/t1/t0 (the symmetric swap puts `&mfa_regs`
  back into mscratch), and executes `mret`. The handler touches only
  t0/t1/t2, all saved and restored, so a0/a1 survive untouched.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make mtval-fault-address.elf` (added to `all` in the Makefile).
Run: `timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mtval-fault-address.elf`
with `~/workspace/qemu/usr/bin/qemu-system-riscv64` (8.2.2) and
`LD_LIBRARY_PATH=~/workspace/qemu/usr/lib/x86_64-linux-gnu:~/workspace/qemu/lib/x86_64-linux-gnu`
(the compat-bin QEMU is broken; missing libfdt/libfuse3).

## Configuration under test

- Hart: hart 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- The two faulting accesses are the only possible traps: `mie` reads
  `0x0` at boot (asserted) and `mstatus.MIE` is cleared explicitly
  before the sequence, so no interrupt source can fire.
- Both fault sites assemble with `.option norvc`, so each faulting
  instruction is exactly 4 bytes. Verified in the disassembly: the
  faulting `lw a0,0(a1)` sits at `0x80000326` with encoding
  `0x0005a503`, and the faulting `sw a0,0(a1)` sits at `0x8000036e`
  with encoding `0x00a5a023`. The program asserts the 4-byte
  resume-minus-fault delta at run time rather than trusting the
  disassembly.
- The faulting instruction's address and the resume address are
  captured with in-asm numeric local labels (`la reg, 1f`,
  `la reg, 2f`), not with C labels-as-values, per the documented
  13.2.0 `&&label` miscompile. The addresses the assertions compare
  against come from the assembler's own label resolution.
- The unmapped address is `0xffffffffc0000000`, produced by
  `li a1, 0xC0000000` (sign-extends on RV64); the same register
  value feeds both the load and the store, and a1 is read back
  unchanged after each trap, proving the handler did not disturb it.

## Sequence and controls

1. Setup: mtvec written with the handler address, read back and
   required to be direct mode; mie required 0.
2. The load asm block runs: `la` captures `fault` (label 1) and
   `resume` (label 2); the resume address is stored into the load
   record (mfa_regs[3]) before the fault; a0 gets the sentinel
   `0xa0a0a0a0a0a0a0a0`; a1 gets the unmapped address; the
   `lw a0, 0(a1)` traps with a load access fault; the handler
   records the triple into record 0, bumps the counter to 1, and
   mrets to label 2; a0/a1 are read back to C.
3. The store asm block runs the identical sequence with
   `sw a0, 0(a1)`, the resume address stored into the store record
   (mfa_regs[7]); the handler records the triple into record 1,
   bumps the counter to 2, and mrets.
4. Checks: trap count exactly 2; load mcause == 5, store mcause == 7;
   each mepc equals its captured fault address; each resume-minus-
   fault delta == 4; the 32-bit word at each fault address equals the
   expected encoding (`0x0005a503` for the lw, `0x00a5a023` for the
   sw); both mtvals equal `0xffffffffc0000000`; the two mtvals are
   equal to each other; a0 still holds its sentinel after the load
   (the faulting load never committed) and after the store; a1 still
   holds the unmapped address after both traps (the handler left it
   alone).

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

| value | run1 | run2 | run3 |
|---|---|---|---|
| mtvec (handler installed, direct) | 0x800001c8 | 0x800001c8 | 0x800001c8 |
| mie at boot (expect 0x0) | 0x0 | 0x0 | 0x0 |
| trap count (expect 2) | 2 | 2 | 2 |
| load: fault addr | 0x80000326 | 0x80000326 | 0x80000326 |
| load: resume addr | 0x8000032a | 0x8000032a | 0x8000032a |
| load: resume - fault (expect 4) | 4 | 4 | 4 |
| load trap: mcause (expect 0x5) | 0x5 | 0x5 | 0x5 |
| load trap: mepc (expect fault addr) | 0x80000326 | 0x80000326 | 0x80000326 |
| load trap: mtval | 0xffffffffc0000000 | 0xffffffffc0000000 | 0xffffffffc0000000 |
| load: insn at fault (expect 0x5a503) | 0x5a503 | 0x5a503 | 0x5a503 |
| store: fault addr | 0x8000036e | 0x8000036e | 0x8000036e |
| store: resume addr | 0x80000372 | 0x80000372 | 0x80000372 |
| store: resume - fault (expect 4) | 4 | 4 | 4 |
| store trap: mcause (expect 0x7) | 0x7 | 0x7 | 0x7 |
| store trap: mepc (expect fault addr) | 0x8000036e | 0x8000036e | 0x8000036e |
| store trap: mtval | 0xffffffffc0000000 | 0xffffffffc0000000 | 0xffffffffc0000000 |
| store: insn at fault (expect 0xa5a023) | 0xa5a023 | 0xa5a023 | 0xa5a023 |
| mtval equal (load vs store) | yes | yes | yes |
| a0 after load (expect sentinel 0xa0...) | 0xa0a0a0a0a0a0a0a0 | 0xa0a0a0a0a0a0a0a0 | 0xa0a0a0a0a0a0a0a0 |
| a1 after both (expect 0xffffffffc0000000) | 0xffffffffc0000000 | 0xffffffffc0000000 | 0xffffffffc0000000 |
| QEMU exit code | 0 | 0 | 0 |
| RESULT | PASS | PASS | PASS |

Runs 1 and 2 are byte-identical; run 3 matches every printed value
and differs only by a single stray carriage-return byte on the
store-trap line (a QEMU UART artifact, visible in the raw log).

What each value means:

- `mcause = 0x5` for the load and `0x7` for the store are the
  architecturally defined access-fault codes for the two access
  types; both faults came from the intended instructions, confirmed
  by the `insn` readbacks (`0x5a503` = `lw a0,0(a1)`,
  `0xa5a023` = `sw a0,0(a1)`) and by `mepc` equaling the
  assembler-captured fault address in both cases.
- `mtval = 0xffffffffc0000000` for both traps: mtval reports the
  exact same faulting address for the load access fault and the
  store access fault, and that address is exactly the value the
  `li` put into a1 (a1 reads back unchanged after both traps,
  confirming neither the fault nor the handler disturbed it). This
  is the module's one question, answered three times.
- Trap count exactly 2 with mie == 0 and MIE clear: each fault was
  taken once and resumed past exactly once; a re-executed fault
  would have made the count 3 and failed the run.
- `a0 = 0xa0a0a0a0a0a0a0a0` after the load: the faulting load never
  committed its destination register.
- QEMU exit code 0 on all runs: the finisher shutdown path
  executed, i.e. `RESULT: PASS` with no parked FAIL.

## Toolchain note (measured, not assumed)

Built with `riscv64-unknown-elf-gcc` 15.2.0 (xPack, via
`~/workspace/toolchains/compat-bin`) with the repo Makefile flags
(`-Wall -Wextra -O2 -march=rv64imac_zicsr -mabi=lp64
-mcmodel=medany`). The build log in `bench-logs/build.log`
records the exact commands; zero warnings, only the linker's benign
RWX-LOAD-segment warning also seen on the sibling builds. No
labels-as-values are used anywhere in the module, per the
documented 13.2.0 `&&label` miscompile: the fault/resume addresses
are taken with in-asm numeric local labels (`la reg, 1f` /
`la reg, 2f`), which the assembler resolves exactly, and the
resume address reaches the handler through the save area, not
through a C-level address.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's trap and CSR models on the `virt`
  machine, not real silicon. The mtval-on-access-fault behavior is
  architectural (mtval holds the faulting address for access
  faults), but the unmapped address, the fault cause codes for it,
  and the mepc layout are emulator- and machine-specific.
- Only hart 0, only M-mode, only the load-access-fault and
  store-access-fault paths at one unmapped address. Misaligned
  faults, instruction-fetch faults, S-mode delegation, vectored
  mtvec, and multi-hart behavior are not tested here; the module
  is deliberately that small.
- The three runs are value-identical; no host-varying values appear
  in the check path.

## Reproduction

```
make mtval-fault-address.elf
export LD_LIBRARY_PATH="$HOME/workspace/qemu/usr/lib/x86_64-linux-gnu:$HOME/workspace/qemu/lib/x86_64-linux-gnu"
timeout 30 ~/workspace/qemu/usr/bin/qemu-system-riscv64 -machine virt -nographic -bios none -kernel mtval-fault-address.elf
```

Linked flat at 0x80000000 via `link.ld`. Build log:
`bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`.
Each ends with `RESULT: PASS (traps=2)` and the finisher shutdown
(exit 0).
