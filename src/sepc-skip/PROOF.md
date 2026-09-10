<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: S-mode trap handler adds 4 to sepc, faulting load skipped exactly once (backlog item "riscv sepc-resume-skip")

## What was built

`src/sepc-skip/`: a bare-metal RV64 program that verifies a
supervisor-mode trap handler which adds 4 to sepc before `sret`
resumes the hart at the instruction immediately after a faulting
instruction, skipping the fault exactly once. This is the S-mode
complement to the shipped M-mode `src/mepc-resume-skip` module.
Exactly one mechanism is under test: the S-mode handler's
`sepc += 4` resume. Three files, sharing only `src/boot.S` and
`src/uart.c` with the other demos.

- `ss_main.c`: runs in M-mode at boot. It installs a direct-mode
  stvec handler, points sscratch at the scratch area, installs a
  park loop on mtvec as a safety net (no M-mode trap is expected),
  clears `mstatus.MIE`, asserts `satp == 0` (Bare), asserts
  `mie == 0`, delegates supervisor load faults to S-mode via
  medeleg bits 5 and 13, opens the address space to S-mode with one
  PMP NAPOT R/W/X entry, then executes one volatile asm block that
  writes the S-mode payload entry to mepc, sets `mstatus.MPP=01`,
  and `mret`s into S-mode. The S-mode payload captures the faulting
  instruction's address and the address of the instruction right
  after it with in-asm numeric local labels (`la reg, 2f` /
  `la reg, 3f`), loads a sentinel into a0 and an unmapped address
  into a1, executes one `lw a0, 0(a1)`, and at label 3 writes a
  marker word to memory. After the handler's sret resumes at label
  3, the C code (now running in S-mode, using only memory and MMIO)
  prints the handler-recorded scause/stval/sepc-before/sepc-after
  and trap count, the captured fault/resume addresses, the
  instruction word at the fault address, and the post-resume
  marker/a0/a1 values, and asserts every check listed below.
  `RESULT: PASS` prints only when every check holds. On PASS the
  machine shuts down through the virt test-device finisher (QEMU
  exits 0); on FAIL the hart parks without touching the finisher.
- `ss_trap.S`: S-mode trap entry that swaps t0 with sscratch,
  records scause, stval, and the sepc the hardware saved at trap
  entry into `ss_regs[0..2]`, bumps the trap counter in
  `ss_regs[3]`, advances sepc by 4, records the adjusted sepc in
  `ss_regs[4]`, restores t0 via the symmetric second swap, and
  executes `sret`. The handler touches only t0 (scratch-swapped)
  and t1; a0/a1 are untouched so the post-resume code can verify
  the faulting load never committed. Also defines `ss_mtrap_park`,
  the M-mode safety-net vector (wfi loop, never exercised in a
  passing run).
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make sepc-resume-skip.elf` (added to `all` and `clean` in
the Makefile).
Run: `timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel sepc-resume-skip.elf`
with `~/workspace/qemu/usr/bin/qemu-system-riscv64` (8.2.2) and
`LD_LIBRARY_PATH=~/workspace/qemu/usr/lib/x86_64-linux-gnu:~/workspace/qemu/lib/x86_64-linux-gnu`
(the compat-bin QEMU is broken; missing libfdt/libfuse3).

## A note on the expected scause (backlog said 0xd, measured 0x5)

The backlog item specified `scause = 0xd` (13, load page fault).
The measured value on QEMU 8.2.2 virt is `scause = 0x5` (5, load
access fault), and the program asserts 5, not 13. Reason: the run
executes with `satp = 0` (Bare, read and asserted at boot), so no
page walk occurs; a load to an unmapped physical address raises a
load access fault, not a load page fault. This matches the M-mode
sibling module exactly (`src/mepc-resume-skip` measured mcause=5
for the same unmapped address `0xffffffffc0000000`). Both medeleg
bits 5 and 13 are delegated (readback `0x2020`, both stick); the
handler reports whichever cause fires, and it fires 5. A page
fault (13) would require Sv39 enabled with a page table, which is
a different module's mechanism (`src/sv39/`), not this one.

## Configuration under test

- Hart: hart 0, single hart. QEMU boots the ELF into M-mode with
  `-bios none`; the program drops to S-mode itself via mret.
- The faulting load is the only possible trap: `mie` reads `0x0`
  at boot (asserted), `mstatus.MIE` is cleared explicitly,
  `satp` reads `0x0` (asserted), and `sie` is 0 at boot, so no
  interrupt source can fire and no M-mode trap is expected
  (mtvec points at a park loop as a safety net).
- The fault site assembles with `.option norvc`, so the faulting
  instruction is exactly 4 bytes. Verified in the disassembly: the
  faulting `lw a0,0(a1)` sits at `0x800003e2` with encoding
  `0x0005a503` (4 bytes), and the first marker instruction (`lui
  t0`) sits at `0x800003e6`, exactly 4 bytes later. Without norvc
  the assembler could have picked the 2-byte `c.lw` form for these
  registers, which would have made a fixed +4 skip wrong; the
  program asserts the 4-byte delta from the captured addresses at
  run time rather than trusting the disassembly.
- The faulting instruction's address and the resume address are
  captured with in-asm numeric local labels (`la reg, 2f`,
  `la reg, 3f`), not with C labels-as-values, per the documented
  13.2.0 `&&label` miscompile. The addresses the assertions
  compare against come from the assembler's own label resolution.
- The S-mode payload entry itself is also reached exactly: mepc
  is written with `la t0, 1f` (numeric local label) before the
  mret, and the disassembly confirms mepc's target `0x800003ae`
  is the first payload instruction.

## Sequence and controls

1. Setup (M-mode): stvec written with the S-mode handler address,
   read back and required to be direct mode; satp required 0;
   mie required 0; medeleg bits 5 and 13 set via csrs, read back
   `0x2020`, bit 5 required set; one PMP NAPOT R/W/X entry over
   the whole address space (without it the first S-mode fetch
   faults); sscratch pointed at `ss_regs`.
2. The single asm block runs: mepc = payload entry (label 1),
   `mstatus.MPP=01`, mret enters S-mode at label 1; `la` captures
   `fault` (label 2) and `resume` (label 3); a0 gets the sentinel
   `0xa0a0a0a0a0a0a0a0`; a1 gets the unmapped address
   `0xffffffffc0000000` (`li a1, 0xC0000000` sign-extends on RV64);
   the `lw a0, 0(a1)` traps with a load access fault, delegated
   to S-mode; the handler records scause/stval/sepc-at-entry,
   bumps the counter, records sepc+4, and srets; execution resumes
   at label 3, which writes `0xdeadbeefdeadbeef` to the marker
   slot, then reads a0 and a1 back to C.
3. Checks (run in S-mode, memory/MMIO only): trap count exactly
   1; scause == 5 (load access fault); stval ==
   0xffffffffc0000000; sepc-at-entry == captured fault address;
   sepc-after - sepc-before == 4; sepc-after == captured resume
   address; resume - fault == 4; the 32-bit word at the fault
   address == 0x0005a503 (`lw a0, 0(a1)`); the marker slot holds
   the marker (the resume instruction ran); a0 still holds its
   sentinel (the faulting load never committed); a1 still holds
   the unmapped address (the handler did not clobber it).

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

The three run logs are byte-identical (md5
`003b50b3856f359d046d8c0053b72643`); the table shows run1, and
runs 2 and 3 matched every value.

| step | run1 | run2 | run3 |
|---|---|---|---|
| stvec (handler installed, direct) | 0x800001c8 | 0x800001c8 | 0x800001c8 |
| satp at boot (expect 0x0, Bare) | 0x0 | 0x0 | 0x0 |
| mie at boot (expect 0x0) | 0x0 | 0x0 | 0x0 |
| medeleg after csrs 5+13 | 0x2020 | 0x2020 | 0x2020 |
| captured fault addr | 0x800003e2 | 0x800003e2 | 0x800003e2 |
| captured resume addr | 0x800003e6 | 0x800003e6 | 0x800003e6 |
| resume - fault (expect 4) | 4 | 4 | 4 |
| trap count (expect 1) | 1 | 1 | 1 |
| scause (expect 5, load access fault) | 0x5 | 0x5 | 0x5 |
| stval (expect 0xffffffffc0000000) | 0xffffffffc0000000 | same | same |
| sepc before (expect fault addr) | 0x800003e2 | 0x800003e2 | 0x800003e2 |
| sepc after (expect resume addr) | 0x800003e6 | 0x800003e6 | 0x800003e6 |
| sepc delta (expect 4) | 4 | 4 | 4 |
| insn at fault addr (expect 0x5a503) | 0x5a503 | 0x5a503 | 0x5a503 |
| marker (expect 0xdeadbeefdeadbeef) | 0xdeadbeefdeadbeef | same | same |
| a0 after (expect sentinel 0xa0...) | 0xa0a0a0a0a0a0a0a0 | same | same |
| a1 after (expect 0xffffffffc0000000) | 0xffffffffc0000000 | same | same |
| QEMU exit code | 0 | 0 | 0 |
| RESULT | PASS | PASS | PASS |

What each value means:

- `medeleg = 0x2020`: bits 5 (load access fault) and 13 (load
  page fault) both delegated to S-mode, read back from the CSR,
  not assumed. Bit 5 is required set by the setup check.
- `scause = 0x5` (5): load access fault, the correct code for a
  load from an unmapped address with satp=Bare (no page walk).
  `stval = 0xffffffffc0000000` is the faulting virtual address,
  exactly the sign-extended value the `li` put into a1 (a1 reads
  back the same value after resume, confirming the handler did
  not disturb it).
- `sepc before = 0x800003e2` equals the captured fault address
  from the assembler's label, read from the CSR by the handler,
  not inferred. `insn = 0x5a503` confirms the trapping
  instruction is the intended `lw a0, 0(a1)` (matches the
  disassembly `0x0005a503`).
- `sepc after = 0x800003e6` equals the captured resume address,
  and the delta is exactly 4 in both the handler's records and the
  independent `la` captures: two separate measurements of the
  same +4 advance agree.
- `marker = 0xdeadbeefdeadbeef`: the store at label 3 ran, so the
  hart resumed at faulting+4 and executed the very next
  instruction. A +8 advance would have skipped the marker; a
  +0 advance would have re-trapped.
- Trap count exactly 1 with mie == 0 and sie == 0: the fault was
  taken once and never re-delivered. Had the handler resumed at
  the faulting instruction, the second trap would have made the
  count 2 and failed the run.
- `a0 = 0xa0a0a0a0a0a0a0a0` after resume: the faulting load never
  committed its destination register. QEMU did not write a0 on the
  faulting access, and the handler left a0 alone, so the sentinel
  surviving is direct evidence the fault was skipped, not
  executed.
- QEMU exit code 0 on all runs: the finisher shutdown path
  executed, i.e. `RESULT: PASS` with no parked FAIL.

## Toolchain note (measured, not assumed)

Built with the distro `riscv64-unknown-elf-gcc` 13.2.0 via the
repo Makefile flags (`-Wall -Wextra -O2 -ffreestanding -nostdlib
-nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr
-mabi=lp64 -mcmodel=medany`). The build log in
`bench-logs/build.log` records the exact commands; zero warnings,
only the linker's benign RWX-LOAD-segment warning also seen on
the sibling builds. No labels-as-values are used anywhere in the
module, per the documented 13.2.0 `&&label` miscompile: the
fault/resume addresses and the S-mode payload entry are taken
with in-asm numeric local labels (`la reg, 1f` / `2f` / `3f`),
which the assembler resolves exactly, and the handler computes
the resume as `sepc + 4` in the trap entry itself.

Environment incident, recorded honestly: mid-run the distro
cross-compiler vanished from PATH (the first successful build
used it; a rebuild minutes later failed with `riscv64-unknown-elf-gcc:
No such file or directory`). The package was reinstalled from the
local apt cache (`gcc-riscv64-unknown-elf 13.2.0-11ubuntu1+12` and
`binutils-riscv64-unknown-elf 2.42-1ubuntu1+6`), and the module was
then rebuilt from clean object files with the reinstalled 13.2.0;
the build log below is that genuine clean rebuild, and all three
QEMU runs above are against the resulting binary. The cause of the
disappearance is unknown; nothing in this module depends on it.

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's trap and CSR models on the `virt`
  machine, not real silicon. The `sepc += 4` skip semantics are
  architectural (sepc holds the faulting instruction's address for
  synchronous traps, and `sret` resumes at sepc), but the fault
  cause code for this address (5, load access fault) and the
  unmapped address itself are emulator- and machine-specific.
- Only hart 0, only the S-mode synchronous load-fault path, only
  satp=Bare. Store faults, misaligned faults, page faults under
  Sv39, interrupt-driven entry, vectored stvec, and multi-hart
  behavior are not tested here; the module is deliberately that
  small.
- The +4 skip is correct only because the faulting instruction is
  exactly 4 bytes; the program asserts the 4-byte delta at run
  time, but a 2-byte compressed faulting instruction would need a
  different advance, which this module does not attempt.
- The three runs are byte-identical; no host-varying values appear
  in the check path.

## Reproduction

```
make sepc-resume-skip.elf
export LD_LIBRARY_PATH="$HOME/workspace/qemu/usr/lib/x86_64-linux-gnu:$HOME/workspace/qemu/lib/x86_64-linux-gnu"
timeout 30 ~/workspace/qemu/usr/bin/qemu-system-riscv64 -machine virt -nographic -bios none -kernel sepc-resume-skip.elf
```

Linked flat at 0x80000000 via `link.ld`. Build log:
`bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`.
Each ends with `RESULT: PASS (traps=1)` and the finisher shutdown
(exit 0).

## Build log (verbatim)

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sepc-skip/ss_trap.S -o src/sepc-skip/ss_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sepc-skip/ss_main.c -o src/sepc-skip/ss_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o sepc-resume-skip.elf src/boot.o src/uart.o src/sepc-skip/ss_trap.o src/sepc-skip/ss_main.o
/usr/lib/gcc/riscv64-unknown-elf/13.2.0/../../../riscv64-unknown-elf/bin/ld: warning: sepc-resume-skip.elf has a LOAD segment with RWX permissions
```

## Run logs (verbatim, all three byte-identical)

run1.log:

```
sepc-resume-skip: S-mode handler adds 4 to sepc, fault skipped once
setup: stvec=0x800001c8 satp=0x0 mie=0x0
setup: medeleg=0x2020
setup complete; dropping to S-mode...
addrs: fault=0x800003e2 resume=0x800003e6 delta=4
trap: count=1 scause=0x5 stval=0xffffffffc0000000
sepc: before=0x800003e2 after=0x800003e6 delta=4 insn@sepc_before=0x5a503
post: marker=0xdeadbeefdeadbeef a0=0xa0a0a0a0a0a0a0a0 a1=0xffffffffc0000000
RESULT: PASS (traps=1)
```

run2.log: byte-identical to run1.log (md5 003b50b3856f359d046d8c0053b72643).

run3.log: byte-identical to run1.log (md5 003b50b3856f359d046d8c0053b72643).
