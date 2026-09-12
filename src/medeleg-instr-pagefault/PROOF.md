<!-- PROOF-HEADER
Checks: 32
Mismatches: 0
Checksum: 0xe4760397517dbcf6
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: medeleg bit-12 instruction-page-fault trap destination switch (backlog item "riscv medeleg-instr-pagefault")

## What was built

`src/medeleg-instr-pagefault/`: a bare-metal RISC-V program that
executes the same S-mode jump to an unmapped page in two phases and
checks the trap destination flips with `medeleg` bit 12. M-mode
builds a minimal Sv39 page table by hand: `root[2]` -> `l1_id[0]` ->
`l0_id[]`, identity-mapping `[0x80000000, 0x80080000)` with R|W|X and
leaving `l0_id[128]` zero (invalid), so an instruction fetch from the
fault page at `0x80080000` dies at the leaf lookup. One PMP NAPOT
entry grants S-mode R|W|X over the whole address space; Sv39 is
enabled via `satp` + `sfence.vma` before the drop to S-mode.

Phase A runs with `medeleg` bit 12 set (readback `0x1000` on QEMU
8.2.2): the S-mode jump to the fault page must trap in S-mode with
`scause = 0xc` (instruction page fault), `sepc` at the faulting
fetch address, and `stval` holding the faulting address, while the
M-mode handler sees only the phase-A return `ecall`. Phase B runs
with bit 12 clear (readback `0x0`): the same jump must trap in
M-mode with `mcause = 0xc` and `mepc` at the faulting address, with
the S-mode trap count unchanged. Four files, sharing only
`src/boot.S` and `src/uart.c` with the other demos. Exactly one
mechanism is under test: the destination of a supervisor-mode
instruction-page-fault trap as `medeleg` bit 12 flips.

- `mipf_trap.S`: M-mode trap entry (records `mcause`/`mepc`/
  `mstatus`/`mtval` in `m_regs`; the only M-mode traps possible are
  the phase-A `ecall` and the phase-B page fault, so the handler
  loads the armed continuation from its save area into `mepc`, sets
  `mstatus.MPP` to M-mode, and `mret`s into it) and S-mode trap
  entry (records `scause`/`sepc`/`sstatus`/`stval` in `s_regs`,
  resumes at the address the payload stored, and `sret`s back).
  Zero continuation or zero resume address parks the hart.
- `mipf_main.c`: two-phase driver. Builds the page tables and
  verifies every PTE by hand, writes `satp`, verifies the `medeleg`
  readbacks, drops M -> S via `sret`, jumps to the fault page from
  S-mode in both phases, publishes every measured value, runs the
  checks, computes an FNV-1a checksum over the verdict values, and
  reports PASS/FAIL. On PASS it writes `0x5555` to the virt
  test-device finisher so the QEMU process exits 0; on FAIL it
  parks the hart.

## Build log

From `bench-logs/build.log` (riscv64-unknown-elf-gcc 13.2.0,
`-O2`, `boot.o` first in link order):

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/boot.S -o src/boot.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/uart.c -o src/uart.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/medeleg-instr-pagefault/mipf_trap.S -o src/medeleg-instr-pagefault/mipf_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/medeleg-instr-pagefault/mipf_main.c -o src/medeleg-instr-pagefault/mipf_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o medeleg-instr-pagefault.elf src/boot.o src/uart.o src/medeleg-instr-pagefault/mipf_trap.o src/medeleg-instr-pagefault/mipf_main.o
```

Zero warnings.

## Run output (QEMU 8.2.2, `qemu-system-riscv64 -machine virt -nographic -bios none -kernel medeleg-instr-pagefault.elf`)

From `bench-logs/run1.log` (runs 1-3 byte-identical, all exit 0):

```
medeleg-instr-pagefault: medeleg bit-12 instruction-page-fault trap destination switch
m-mode: pmpcfg0=0x1f (entry0: NAPOT all R|W|X)
m-mode: satp=0x8000000000080005 (MODE=8 ASID=0)
m-mode: phase-A medeleg write=0x1000 readback=0x1000
m-mode: entering S-mode (phase A, bit 12 set)
medeleg-instr-pagefault: instruction-page-fault trap destination switch test
boot: medeleg=0x0
table: root[2]->l1[0]->l0 identity [0x80000000,0x80080000) R|W|X, l0[128]=0 (fault page 0x80080000)
phaseA: medeleg bit12 set readback=0x1000
phaseB: medeleg bit12 clear readback=0x0
pA: s_traps=1 scause=0xc sepc=0x80080000 expected=0x80080000 stval=0x80080000 sstatus_spp=1
pA: m_traps=1 mcause=0x9 mepc=0x800002e2
pB: m_traps=2 mcause=0xc mepc=0x80080000 expected=0x80080000 mtval=0x80080000 mstatus_mpp=1 s_traps=1
quiet: m_traps=2 s_traps=1
checksum=0xe4760397517dbcf6
RESULT: PASS (checks=32)
```

## What the numbers mean

- `satp=0x8000000000080005`: MODE=8 (Sv39), ASID=0, root PPN
  matches the hand-built table page; `pmpcfg0=0x1f` confirms the
  NAPOT R|W|X grant the S-mode phase needs.
- Phase A (bit 12 set): exactly one S-mode trap, `scause=0xc`,
  `sepc` and `stval` both `0x80080000` (the faulting fetch
  address), arriving from S-mode (`sstatus.SPP=1`). The M-mode
  handler saw exactly one trap and it was the phase-A return
  `ecall` (`mcause=0x9`): the page fault did not leak to M-mode.
- Phase B (bit 12 clear): exactly one further M-mode trap,
  `mcause=0xc`, `mepc` and `mtval` both `0x80080000`, arriving
  from S-mode (`mstatus.MPP=1`); the S-mode trap count stayed 1.
- The 2,000,000-iteration quiet window moved neither counter.
- FNV-1a `0xe4760397517dbcf6` over the ten verdict values
  (per-mode counts, causes, PCs, trap values, both `medeleg`
  readbacks) is byte-identical across all three runs.

## Verification record

- 32 checks, 0 failures, verdict PASS, QEMU exit code 0 on all
  three runs; `diff` of the three raw logs is empty.
- Every PTE the walk depends on was verified by hand before
  `satp` was written: both pointer entries are V-only with the
  exact table PPNs, leaf 0 carries V|R|W|X with PPN `0x80000`,
  leaf 127 is valid, and `l0_id[128]` reads back zero.
- Toolchain: riscv64-unknown-elf-gcc 13.2.0
  (`~/workspace/toolchains/ubuntu-rv64`), QEMU 8.2.2
  (`~/workspace/qemu`), `-bios none`, single hart.
