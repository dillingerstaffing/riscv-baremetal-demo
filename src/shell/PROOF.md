<!-- PROOF-HEADER
Checks: 6
Mismatches: 0
Environment: QEMU
Verdict: PASS
-->

# PROOF: bare-metal UART shell on RISC-V

## What was built

A new module, `src/shell/`, in this repo, built as its own binary
`shell.elf` (shares only `src/boot.S` and the UART driver with the other
demos; `demo.elf`, `preempt.elf`, `virtio-blk.elf`, and `smp.elf` are
untouched and still build).

- `sh_main.c`: installs the trap handler (`mtvec`/`mscratch`), then runs
  the interpreter loop forever.
- `cmds.c`: line reader with echo and backspace handling, fixed 128-byte
  buffer, four commands (`help`, `echo`, `regs`, `uptime`), and an
  explicit error for anything else. `echo` skips the command word and
  prints the remainder exactly as typed.
- `trap.S`: M-mode trap entry. On entry it saves x1-x31 plus
  mepc/mstatus/mcause into a 34-word bank (`trap_bank` in `.bss`), using
  `mscratch` to carry the bank address. For an M-mode ecall
  (mcause 11) it advances mepc past the ecall and resumes with `mret`;
  any other trap parks the hart. The `regs` command issues an `ecall`
  from C, so the bank holds the register state at that instruction.
- `src/uart.c` / `src/uart.h`: added `uart_getc`, the polled receive
  half of the driver (wait for LSR data-ready, read the receive
  buffer at offset 0). No other driver behavior changed.

Makefile: `SHELL_SRCS`, the `shell.elf` target, and a `run-shell` target.
Run under QEMU with the virt board, `-bios none`, `-nographic`
(serial on stdio), scripted input piped on stdin.

## Ground truth the numbers rest on

- The register dump is checked against the ELF, not against itself:
  `mepc` must equal the address of the `ecall` instruction in
  `snapshot_regs` (from `riscv64-unknown-elf-objdump -d shell.elf`),
  `sp` must fall inside the 16 KiB stack window below `_stack_top`
  (from `riscv64-unknown-elf-nm`), and `ra` must point into `.text`.
- `mcause` 11 is the ISA-defined code for an environment call from
  M-mode; the handler only resumes when it sees exactly that value.
- `uptime` reads the CLINT mtime register at 0x0200bff8, the same
  address used by `src/preempt/clint.c`.
- `echo` correctness is checked byte-for-byte with `cmp` against the
  input line.

## Build log

```
$ make shell.elf
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
  -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany \
  -c src/uart.c -o src/uart.o
riscv64-unknown-elf-gcc ... -c src/shell/trap.S -o src/shell/trap.o
riscv64-unknown-elf-gcc ... -c src/shell/sh_main.c -o src/shell/sh_main.o
riscv64-unknown-elf-gcc ... -c src/shell/cmds.c -o src/shell/cmds.o
riscv64-unknown-elf-gcc ... -T link.ld -o shell.elf \
  src/boot.o src/uart.o src/shell/trap.o src/shell/sh_main.o src/shell/cmds.o
ld: warning: shell.elf has a LOAD segment with RWX permissions
```

Zero compiler warnings. (The RWX linker note is the link.ld layout and
appears for the other binaries too.) Full `make` also rebuilds
`demo.elf`, `preempt.elf`, `virtio-blk.elf`, and `smp.elf` cleanly, so
the `uart_getc` addition did not disturb the other modules.

## Captured session

Input file (58 bytes, one command per line):

```
help
echo hello world 123 !@#
echo
regs
uptime
frobnicate
```

Run:

```
$ timeout 6 qemu-system-riscv64 -machine virt -nographic -bios none \
    -kernel shell.elf < shell_input.txt > shell_session.txt
```

QEMU exits by timeout (the shell has no quit command; it serves
forever). Output captured to `shell_session.txt`: 800 bytes. Every
byte below is the real captured output, shown with `cat -A`
(`^M$` = CRLF line endings):

```
uart-shell ready^M$
> help^M$
commands:^M$
  help    list commands^M$
  echo    print text back byte-for-byte^M$
  regs    dump registers saved by the trap handler^M$
  uptime  print mtime ticks since reset^M$
> echo hello world 123 !@#^M$
hello world 123 !@#^M$
> echo^M$
^M$
> regs^M$
ra = 0x80000360^M$
sp = 0x80004a80^M$
gp = 0x0^M$
tp = 0x0^M$
t0 = 0x80000af8^M$
t1 = 0x80000af8^M$
t2 = 0x0^M$
s0 = 0xd^M$
s1 = 0x80000af8^M$
a0 = 0xa^M$
a1 = 0x72^M$
a2 = 0x0^M$
a3 = 0x80000614^M$
a4 = 0x80000a7c^M$
a5 = 0x0^M$
a6 = 0xd^M$
a7 = 0x0^M$
s2 = 0xa^M$
s3 = 0x7f^M$
s4 = 0x8^M$
s5 = 0x1f^M$
s6 = 0x7e^M$
s7 = 0x80000a78^M$
s8 = 0x80000640^M$
s9 = 0x80000968^M$
s10 = 0x80000638^M$
s11 = 0x80000968^M$
t3 = 0x0^M$
t4 = 0x0^M$
t5 = 0x0^M$
t6 = 0x0^M$
mepc    = 0x80000402^M$
mstatus = 0xa00001800^M$
mcause  = 0xb^M$
> uptime^M$
mtime = 194645 ticks^M$
> frobnicate^M$
unknown command: frobnicate^M$
> 
```

## Verification results

- Boot prompt: `uart-shell ready` followed by `> ` printed before any
  input was consumed. Verified.
- `help`: lists exactly the four implemented commands. Verified.
- `echo hello world 123 !@#`: the output line is byte-identical to the
  input text (`cmp` of the captured line against the expected bytes:
  identical). `echo` with no argument prints an empty line. Verified.
- `regs`: `mepc = 0x80000402`, and objdump shows the `ecall` in
  `snapshot_regs` at exactly `0x80000402`. `sp = 0x80004a80` lies inside
  the stack window `[0x80000b00, 0x80004b00)` (`_stack_top = 0x80004b00`
  per nm, 16 KiB below it). `ra = 0x80000360` points into `.text`
  (inside the command dispatch code). `mcause = 0xb` (11), the M-mode
  ecall code. Plausible on all four checks.
- `uptime`: `mtime = 194645 ticks`, a nonzero, monotonically sensible
  CLINT reading. Verified.
- Unknown command `frobnicate`: prints `unknown command: frobnicate`
  and returns to the prompt instead of hanging or crashing. Verified.

## Notes and caveats

- The first build of `cmds.c` read the CLINT base address (0x02000000)
  instead of the mtime register (0x0200bff8). The shell printed the
  prompt, echoed `uptime`, then parked: the bad load trapped with an
  unexpected mcause and the handler's park path (wfi loop) engaged.
  Fixed by using the mtime address, confirmed working in the session
  above. The park-on-unexpected-trap behavior did exactly what it was
  written to do.
- The shell has no quit command; scripted runs rely on `timeout` to
  stop QEMU. The final `> ` in the capture is the prompt waiting for
  the next line when the timeout fired.
- Input is line-buffered with a 128-byte cap; overlong lines are
  truncated at the buffer limit rather than overflowing. Control
  characters other than CR/LF/BS/DEL are ignored.
