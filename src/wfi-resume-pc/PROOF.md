<!-- PROOF-HEADER
Checks: 9
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: WFI resume-PC after a machine software interrupt (backlog item 87)

## What was built

`src/wfi-resume-pc/`: a bare-metal RISC-V program that checks the
trap-return contract at a `wfi` instruction on the QEMU `virt` board.
Two files, about 400 lines total, sharing only `src/boot.S` and
`src/uart.c` with the other demos. Exactly one mechanism is under
test: when a machine software interrupt trap is taken with `mepc`
pointing at a `wfi`, the handler clears the source, advances the saved
`mepc` past the `wfi`, and `mret` resumes at `wfi`+4 with every
general-purpose register bit-identical to its pre-trap value.

- `rpc_trap.S`: M-mode trap entry. Saves all of x1-x31 (t0 via the
  `mscratch` swap), records `mcause`/`mepc` into C-visible globals,
  clears the CLINT msip bit with a 32-bit store, adds 4 to the saved
  `mepc`, bumps the trap counter, then restores every register and
  executes `mret`. There is deliberately no C call anywhere on the
  trap path: any compiler-generated prologue would touch the very
  registers the module compares.
- `rpc_main.c`: installs direct-mode `mtvec`, enables only
  `mie.MSIE`, then runs three identical runs. Each run snapshots
  x1-x31, captures the exact `wfi` address with `la x5, 1f` (a numeric
  asm local label; the assembler resolves it exactly, unlike a C
  labels-as-values address, which riscv gcc 13.2.0 misplaces at -O2),
  sets msip with MIE clear, enables MIE so the pending interrupt traps
  immediately with `mepc` at the `wfi`, captures the resume pc at the
  label after the `wfi`, and snapshots x1-x31 again. Every check is
  computed in code; `RESULT: PASS` prints only when all hold.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make wfi-resume-pc.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel wfi-resume-pc.elf`
(or `make run-wfi-resume-pc`).

## Configuration under test

- Hart: mhartid = 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- CLINT msip for hart 0 at `0x02000000`, accessed as a 32-bit register
  (QEMU 8.2.2's CLINT model only implements 4-byte msip accesses; a
  64-bit access faults, verified while building the msip module).
- Only the machine software interrupt is enabled (`mie` bit 3); no
  timer, no external interrupts, no PLIC involvement.

## Sequence and controls

1. Control: with `mie.MSIE` and `mstatus.MIE` set but msip reading 0,
   a 1M-spin quiet window must produce zero traps. This proves the
   runs' traps come from the msip write and not from spurious
   delivery.
2. Per run: with MIE clear, msip is set to 1 and read back (must be
   1). One volatile asm block then snapshots x1-x31, spills t0/t1,
   captures the `wfi` address via `la x5, 1f`, restores t0, and
   executes `csrsi mstatus, 8`. The interrupt is already pending, so
   the trap is taken at the next instruction boundary with `mepc`
   equal to the `wfi` address; the `wfi` itself never executes (see
   the probe note below).
3. The handler records `mcause`/`mepc`, clears msip, advances the saved
   `mepc` by 4, and `mret`s. Execution resumes at the label after the
   `wfi`; its address is captured as resume_pc, t0/t1 are restored
   from the spill slots, and x1-x31 are snapshotted again.
4. Checks per run: exactly one trap fired, `mcause` is
   `0x8000000000000003` (interrupt bit set, code 3 = machine software
   interrupt), the trap's `mepc` equals the captured `wfi` address,
   resume_pc equals `wfi` address + 4, msip reads 0 afterwards, and
   all 31 before/after register pairs agree (any mismatch is printed
   with the register name and both values).

Probe note: an early probe build whose handler did not advance `mepc`
hung forever after `mret`: it landed back on the `wfi` with msip
already clear, and QEMU suspended the vcpu with no interrupt ever
coming. That hang is the evidence that the trap is taken before the
`wfi` executes (mepc at the `wfi`, not after it); the final handler
advances `mepc` by 4 so the resume lands past the `wfi`.

Disassembly check (in `bench-logs/build.log` via objdump): the
`la x5, 1f` assembled to `addi t0,t0,20` resolving to exactly
`0x80000406`, the address of the `wfi` (`10500073`, 4 bytes, no
16-bit form exists for SYSTEM instructions), and the resume label to
`0x8000040a` = `wfi`+4.

## Measured numbers

Three QEMU invocations, three runs each (nine runs total). Every run
reported identical values:

| run | wfi_addr     | trap_mepc    | resume_pc    | register diffs |
|-----|--------------|--------------|--------------|----------------|
| 0-2 | `0x80000406` | `0x80000406` | `0x8000040a` | 0/31           |

- `mcause` was `0x8000000000000003` on all nine traps (checked in
  code; a mismatch fails the run).
- Trap count advanced by exactly 1 per run; the control window saw 0
  traps with msip clear on all three invocations.
- msip read 0 after every run (handler cleared it); no re-delivery.
- All three QEMU processes exited 0 via the virt test-device
  finisher, which the program writes only on `RESULT: PASS`.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's interrupt and `wfi` handling on the
  `virt` machine, not real silicon.
- The `wfi` instruction never actually suspends in this test: the
  interrupt is already pending when MIE is set, so the trap is taken
  at the instruction boundary before the `wfi` executes. What is
  verified is the trap-return contract at a `wfi` instruction
  (`mepc` == `wfi` address, `mret` resumes at `wfi`+4 after the
  handler advances `mepc`, registers intact). A genuine
  sleep-then-IPI-wake would need a second hart or a timer to assert
  the interrupt mid-sleep; that is a different, larger experiment and
  is out of scope here.
- Only hart 0, only M-mode, only the machine software interrupt.
  Timer interrupts, S-mode delegation, and multi-hart IPIs are not
  tested; the module is deliberately that small.
- The register comparison covers x1-x31; x0 is hardwired to zero by
  the ISA and is not compared.
- The addresses (`0x80000406`/`0x8000040a`) are link addresses of
  this build, not architectural constants; the invariants that
  transfer are `trap_mepc == wfi_addr` and
  `resume_pc == wfi_addr + 4`, re-checked by the program on every
  run.

## Reproduction

```
make wfi-resume-pc.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel wfi-resume-pc.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2.
Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`
(each ends with `done`; on PASS the program writes the virt
test-device finisher so QEMU exits 0, on FAIL it parks the hart and
the `timeout` exit status marks the failure).
