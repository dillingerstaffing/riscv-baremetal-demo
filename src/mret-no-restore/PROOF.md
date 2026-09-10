# Proof: mret with no register restore leaves handler clobbers observable (backlog item "riscv mret-no-restore")

## What was built

`src/mret-no-restore/`: a bare-metal RV64 M-mode program that
verifies a trap handler which does not restore registers leaves its
clobbers observable to the pre-trap caller. Exactly one mechanism is
under test: the handler writes a new value set into a0..a7 and
returns with `mret` without restoring them, so the pre-trap code
sees the handler's values, not its own. Four files, sharing only
`src/boot.S` and `src/uart.c` with the other demos.

- `mnr_main.c`: installs a direct-mode mtvec handler, verifies
  `mie == 0` at boot and clears `mstatus.MIE` so the ecall is the
  only trap the run can take, then executes a single volatile asm
  block that loads eight distinct sentinels into a0..a7, records
  them to memory, issues one `ecall`, and reads a0..a7 back to
  memory after `mret` returns. It prints the handler-recorded
  mcause/mepc/mtval and trap count, the before/after dump of all
  eight registers, and asserts every register changed from its
  pre-trap value and exactly matches the handler's written value.
  `RESULT: PASS` prints only when every check holds. On PASS the
  machine shuts down through the virt test-device finisher (QEMU
  exits 0); on FAIL the hart parks without touching the finisher.
- `mnr_trap.S`: trap entry that swaps t0 with mscratch, records
  mcause/mepc/mtval into `mnr_regs[0..2]`, bumps the trap counter
  in `mnr_regs[3]`, advances mepc past the ecall, restores t0 via
  the symmetric second swap, writes the POST_A0..POST_A7 sentinel
  set into a0..a7, and executes `mret` with no restore after that
  point (t1 is left clobbered, no C code runs in the handler).
- `mnr_vals.h`: the sixteen sentinel values, included by both the
  .S and the .c so the written values and the checked values cannot
  drift apart.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make mret-no-restore.elf` (added to `all` in the Makefile).
Run: `timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel mret-no-restore.elf`
with `~/workspace/qemu/usr/bin/qemu-system-riscv64` (8.2.2) and
`LD_LIBRARY_PATH=~/workspace/qemu/usr/lib/x86_64-linux-gnu:~/workspace/qemu/lib/x86_64-linux-gnu`
(the compat-bin QEMU is broken; missing libfdt/libfuse3).

## Configuration under test

- Hart: hart 0, single hart, M-mode (QEMU boots the ELF straight
  into M-mode with `-bios none`).
- The ecall is the only possible trap: `mie` reads `0x0` at boot
  (asserted) and `mstatus.MIE` is cleared explicitly before the
  sequence, so no interrupt source can fire.
- The sentinel load, pre-trap record, ecall, and post-trap readback
  are one volatile asm block with a0..a7 in the clobber list, so the
  compiler emits nothing between them and cannot touch a0..a7
  across the trap boundary. Verified in the disassembly: the block
  is `li` fills into a0..a7, eight `sd` stores, one 4-byte `ecall`
  at `0x80000556`, then eight `sd` stores. The handler disassembly
  shows the sentinel writes flowing straight into `mret` at
  `0x800002e4` with no loads or restores between them.
- `.option norvc` is used in both the block and the handler, so the
  ecall is exactly 4 bytes and the handler's `mepc += 4` lands on
  the first post-trap store. The mepc advance is the one concession
  to forward progress: without it `mret` would re-execute the ecall
  and trap forever. It is documented here, not hidden.

## Sequence and controls

1. Setup: mtvec written with the handler address, read back and
   required to be direct mode; mie required 0; the sixteen
   sentinels required pairwise disjoint (static check in C).
2. The single asm block runs: a0..a7 get `0x1111...` through
   `0x8888...`, are stored to `pre[8]`, the ecall traps, the
   handler records mcause/mepc/mtval, bumps the counter, writes
   `0x9999...` through `0x123456789abcdef0` into a0..a7, and mrets
   with no restore.
3. Checks: trap count exactly 1; mcause == 11 (environment call from
   M-mode); mtval == 0; the 32-bit word at the recorded mepc ==
   `0x00000073` (ecall); every `pre[i]` equals the loaded
   sentinel; every `post[i]` equals the handler's sentinel and
   differs from `pre[i]`.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

The three run logs are byte-identical; the table shows run1, and
runs 2 and 3 matched every value.

| step | run1 | run2 | run3 |
|---|---|---|---|
| mtvec (handler installed, direct) | 0x800001c8 | 0x800001c8 | 0x800001c8 |
| mie at boot (expect 0x0) | 0x0 | 0x0 | 0x0 |
| trap count (expect 1) | 1 | 1 | 1 |
| mcause (expect 11, M-mode ecall) | 0xb | 0xb | 0xb |
| mepc | 0x80000556 | 0x80000556 | 0x80000556 |
| mtval (expect 0x0) | 0x0 | 0x0 | 0x0 |
| insn at mepc (expect 0x73) | 0x73 | 0x73 | 0x73 |
| a0 before / after | 0x1111111111111111 / 0x9999999999999999 | same | same |
| a1 before / after | 0x2222222222222222 / 0xaaaaaaaaaaaaaaaa | same | same |
| a2 before / after | 0x3333333333333333 / 0xbbbbbbbbbbbbbbbb | same | same |
| a3 before / after | 0x4444444444444444 / 0xcccccccccccccccc | same | same |
| a4 before / after | 0x5555555555555555 / 0xdddddddddddddddd | same | same |
| a5 before / after | 0x6666666666666666 / 0xeeeeeeeeeeeeeeee | same | same |
| a6 before / after | 0x7777777777777777 / 0xffffffffffffffff | same | same |
| a7 before / after | 0x8888888888888888 / 0x123456789abcdef0 | same | same |
| QEMU exit code | 0 | 0 | 0 |
| RESULT | PASS | PASS | PASS |

What each value means:

- `mcause = 0xb` (11): environment call from M-mode, the correct
  code for an ecall issued in M-mode (the cause table assigns 8 to
  U-mode, 9 to S-mode, 11 to M-mode). The first version of this
  module asserted 8 and failed exactly this check while every
  register pair passed; the measurement corrected the constant,
  not the mechanism. The failure was in the test's assumption, and
  the check now asserts the architecturally correct value.
- `mepc = 0x80000556`: the recorded mepc equals the disassembled
  address of the `ecall` instruction in the single asm block,
  read from the CSR by the handler, not inferred. `insn@mepc =
  0x73` confirms the trapping instruction is ecall.
- All eight `post[i]` values equal the handler's sentinel set and
  differ from all eight `pre[i]` values: the handler's clobbers
  survived `mret` with no restore and are exactly what the
  pre-trap caller observes. The `pre[i]` readbacks equal the loaded
  sentinels, so the record path is verified too.
- Trap count exactly 1 with mie == 0 and MIE clear: the ecall was
  the only trap, taken once, with no re-delivery.
- QEMU exit code 0 on all runs: the finisher shutdown path
  executed, i.e. `RESULT: PASS` with no parked FAIL.

## What was deliberately not restored

Honesty about the handler's exact behavior, since "no restore" is
the claim: t0 is swapped back via the second `csrrw mscratch`
(the symmetric scratch-register save every trap entry needs, not
a restore of a caller-visible value); t1 is left holding mepc+4;
mepc is advanced by 4 for forward progress; a0..a7 are written and
never saved anywhere. The eight registers under test (a0..a7) are
the ones the assertion covers, and the disassembly confirms no
save/restore of them exists on the handler path.

## Toolchain note (measured, not assumed)

Built with the distro `riscv64-unknown-elf-gcc` 13.2.0 via the
repo Makefile flags (`-Wall -Wextra -O2 -march=rv64imac_zicsr
-mabi=lp64 -mcmodel=medany`). The build log in
`bench-logs/build.log` records the exact commands; zero warnings,
only the linker's benign RWX-LOAD-segment warning also seen on
the sibling builds. No labels-as-values are used anywhere in the
module, so the documented 13.2.0 `&&label` miscompile does not
apply (the trap-resume address is `mepc += 4` computed in the
handler, and no trap-resume address is materialized in C).

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's trap and CSR models on the `virt`
  machine, not real silicon. The mret-restore semantics exercised
  here (handler-written registers are not magically restored) are
  architectural: nothing in the privileged spec restores integer
  registers on `mret`.
- Only hart 0, only M-mode, only the synchronous ecall path.
  Interrupt-driven entry, S-mode delegation, vectored mtvec, and
  multi-hart behavior are not tested here; the module is
  deliberately that small.
- The claim covers a0..a7 only. t0/t1/sp/ra and the CSRs are
  outside the asserted surface (t0 is scratch-swapped, t1 is left
  clobbered, neither is checked).
- The three runs are byte-identical; no host-varying values appear
  in the check path.

## Reproduction

```
make mret-no-restore.elf
export LD_LIBRARY_PATH="$HOME/workspace/qemu/usr/lib/x86_64-linux-gnu:$HOME/workspace/qemu/lib/x86_64-linux-gnu"
timeout 30 ~/workspace/qemu/usr/bin/qemu-system-riscv64 -machine virt -nographic -bios none -kernel mret-no-restore.elf
```

Linked flat at 0x80000000 via `link.ld`. Build log:
`bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`.
Each ends with `RESULT: PASS (traps=1)` and the finisher shutdown
(exit 0).
