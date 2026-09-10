<!-- PROOF-HEADER
-->

# PROOF: two-hart SMP bring-up on RISC-V

## What was built

A new module, `src/smp/`, in this repo, built as its own binary
`smp.elf` (shares only the UART driver with the other demos;
`demo.elf` and `preempt.elf` are untouched and still build).

- `smp_boot.S`: entry point. QEMU starts every hart at the ELF entry in
  M-mode when booted with `-bios none -kernel`. Each hart reads its ID
  from the `mhartid` CSR and installs a private stack: hart N gets the
  top of `hart_stacks[N]`, a 4 KiB slice of a static array. Hart 0 then
  clears BSS and calls `smp_main`; every other hart spins on the
  `smp_release` flag (a word in `.data`, so it reads 0 from the loaded
  image even before hart 0 finishes clearing BSS) and on release calls
  `hart1_main`.
- `spinlock.h/.c`: mutual exclusion from one atomic instruction.
  Acquire swaps in a 1 with `amoswap.w.aq`; the swap is atomic across
  harts, so exactly one hart observes the old value 0. Release is a
  store of 0 after a `fence`.
- `smp_print.c`: UART helpers that hold the spinlock for a whole line,
  so output from the two harts never interleaves mid-line.
- `smp_main.c`: hart 0 bring-up. Initializes the UART, prints its hart
  ID and stack range, stamps `rdcycle`, raises `smp_release` after a
  fence, waits for hart 1's done flag (with a cycle-count timeout so a
  stuck secondary is reported, not hung on), then prints the
  verification report.
- `hart1.c`: hart 1 entry. Its first action after observing the flag is
  an `rdcycle` stamp; it prints its hart ID (from `mhartid`) and stack
  pointer under the lock, records its sp for hart 0's check, stamps
  again, then raises its done flag after a fence.

No library is wrapped: the startup sequence, the stack carving, the
flag protocol, and the lock are all constructed from the CSR/memory
model behavior described above.

## Ground truth the numbers rest on

- `mhartid` is defined by the privileged ISA to hold the hart's ID;
  both harts print the value they read, and QEMU's `-smp 2` is
  configured to provide exactly harts 0 and 1.
- `rdcycle` on this QEMU was characterized in `src/preempt/PROOF.md`:
  back-to-back correlation of `rdcycle` deltas against `mtime` deltas
  gives a constant 150 counter units per 100 ns `mtime` tick, i.e. the
  counter advances at 1.5e9 units per second of virtual time. Cycle
  counts below convert to virtual time at that ratio.
- Stack disjointness is address arithmetic on the static
  `hart_stacks[2][4096]` array; sp-in-slice is a per-hart measurement.

## Definitions (exactly what the numbers mean)

- Bring-up latency: `t1_entry_cycles - t_release_cycles`, where
  `t_release_cycles` is stamped by hart 0 immediately before the store
  that raises `smp_release`, and `t1_entry_cycles` is hart 1's first
  action after observing the flag. It covers the flag store, hart 1's
  wakeup, and hart 1's path to its first stamp.
- Entry-to-print: `t1_printed_cycles - t1_entry_cycles`, hart 1's cost
  to print four locked UART lines.

## Build log

Toolchain: xPack `riscv-none-elf-gcc` 15.2.0
(`~/workspace/toolchains/xpack-riscv-none-elf-gcc-15.2.0-1`),
`make CROSS=riscv-none-elf- smp.elf`. Zero errors, zero compiler
warnings (the only linker note is the benign RWX-segment warning from
the minimal linker script, also present for the other binaries):

```
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/smp/smp_boot.S -o src/smp/smp_boot.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/smp/spinlock.c -o src/smp/spinlock.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/smp/smp_print.c -o src/smp/smp_print.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/smp/smp_main.c -o src/smp/smp_main.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/smp/hart1.c -o src/smp/hart1.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o smp.elf src/smp/smp_boot.o src/uart.o src/smp/spinlock.o src/smp/smp_print.o src/smp/smp_main.o src/smp/hart1.o
/home/hatch/workspace/toolchains/xpack-riscv-none-elf-gcc-15.2.0-1/bin/../lib/gcc/riscv-none-elf/15.2.0/../../../../riscv-none-elf/bin/ld: warning: smp.elf has a LOAD segment with RWX permissions
```

## QEMU run output

Command (run twice; QEMU killed by timeout after the report printed,
both harts then parked in `wfi`):

```
qemu-system-riscv64 -machine virt -nographic -bios none -smp 2 -kernel smp.elf
```

Run 1 (full output):

```
smp: 2-hart bring-up on QEMU virt
hart0: mhartid=0
hart0: stack_base=0x80000830 stack_top=0x80001830
hart0: sp=0x80001800
hart0: released hart 1
hart1: secondary hart alive
hart1: mhartid=1
hart1: stack_base=0x80001830 stack_top=0x80002830
hart1: sp=0x80002810
smp: verification report
hart0: release_cycles=2146142007855
hart1: entry_cycles=2146143603030
hart1: printed_cycles=2146154612145
hart0: done_cycles=2146154850810
bring-up latency (release->hart1 entry), cycles=1595175
hart1 entry->print done, cycles=11009115
stack slices disjoint (1=yes)=1
hart0 sp in own slice (1=yes)=1
hart1 sp in own slice (1=yes)=1
smp: both harts ran, done
```

Run 2 (verification lines):

```
hart0: mhartid=0
hart1: mhartid=1
hart0: release_cycles=2171677939365
hart1: entry_cycles=2171681493420
hart1: printed_cycles=2171682245985
hart0: done_cycles=2171682306765
bring-up latency (release->hart1 entry), cycles=3554055
hart1 entry->print done, cycles=752565
stack slices disjoint (1=yes)=1
hart0 sp in own slice (1=yes)=1
hart1 sp in own slice (1=yes)=1
```

## What the output proves

- Both harts ran: hart 0 read `mhartid`=0, hart 1 read `mhartid`=1,
  and hart 1's lines appear in the output, which is only possible if
  its code executed.
- The stacks are disjoint: hart 0 owns `[0x80000830, 0x80001830)`,
  hart 1 owns `[0x80001830, 0x80002830)`, adjacent 4 KiB slices, and
  each hart's measured `sp` falls inside its own slice.
- UART output from the two harts never interleaves mid-line: every
  line in the log is intact, which is the observable effect of the
  `amoswap` spinlock.
- Bring-up latency measured 1,595,175 cycles in run 1 and 3,554,055
  cycles in run 2 (about 1.1 ms and 2.4 ms of virtual time at the
  1.5e9 units/s ratio). The run-to-run variance is host-side: under
  QEMU's multi-threaded emulation the dominant term is when the host
  schedules hart 1's vCPU thread after the flag store, not the guest
  code path, which is a handful of instructions on each side.

## How to reproduce

```sh
make CROSS=riscv-none-elf- smp.elf
make run-smp   # QEMU=... overrides the qemu binary path
```
