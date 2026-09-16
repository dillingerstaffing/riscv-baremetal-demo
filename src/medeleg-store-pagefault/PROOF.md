<!-- PROOF-HEADER
Checks: 34
Mismatches: 0
Checksum: 0x5f72e2349de7d1fa
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: medeleg bit-15 store-page-fault trap destination switch (backlog item "riscv medeleg-store-pagefault")

## What was built

`src/medeleg-store-pagefault/`: a bare-metal RISC-V program that
executes the same S-mode store to an unmapped page in two phases
and checks the trap destination flips with `medeleg` bit 15.
M-mode builds a minimal Sv39 page table by hand: `root[2]` ->
`l1_id[0]` -> `l0_id[]`, identity-mapping `[0x80000000, 0x80080000)`
with R|W|X and leaving `l0_id[128]` zero (invalid), so a store to
the fault page at `0x80080000` dies at the leaf lookup. One PMP
NAPOT entry grants S-mode R|W|X over the whole address space;
Sv39 is enabled via `satp` + `sfence.vma` before the drop to
S-mode.

Phase A runs with `medeleg` bit 15 set (readback `0x8000` on QEMU
8.2.2): the S-mode store to the fault page must trap in S-mode
with `scause = 0xf` (store/AMO page fault), `sepc` at the faulting
store instruction, and `stval` holding the faulting data address,
while the M-mode handler sees only the phase-A return `ecall`.
Phase B runs with bit 15 clear (readback `0x0`): the same store
must trap in M-mode with `mcause = 0xf`, `mepc` at the faulting
store, and `mtval` at the fault address, with the S-mode trap count
unchanged. Five files, sharing only `src/boot.S` and `src/uart.c`
with the other demos. Exactly one mechanism is under test: the
destination of a supervisor-mode store-page-fault trap as
`medeleg` bit 15 flips.

- `mspf_trap.S`: M-mode trap entry (records `mcause`/`mepc`/
  `mstatus`/`mtval` in `m_regs`; the only M-mode traps possible
  are the phase-A `ecall` and the phase-B page fault, so the
  handler loads the armed continuation from its save area into
  `mepc`, sets `mstatus.MPP` to M-mode, and `mret`s into it) and
  S-mode trap entry (records `scause`/`sepc`/`sstatus`/`stval` in
  `s_regs`, resumes at the address the payload stored, and
  `sret`s back). Zero continuation or zero resume address parks the
  hart.
- `mspf_main.c`: two-phase driver. Builds the page tables and
  verifies every PTE by hand, writes `satp`, verifies the
  `medeleg` readbacks, drops M -> S via `sret`, stores to the
  fault page from S-mode in both phases, publishes every measured
  value, runs the checks, computes an FNV-1a checksum over the
  verdict values, and reports PASS/FAIL. On PASS it writes
  `0x5555` to the virt test-device finisher so the QEMU process
  exits 0; on FAIL it parks the hart.

## Build log

From `bench-logs/build.log` (riscv64-unknown-elf-gcc 13.2.0,
`-O2`, `boot.o` first in link order):

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/medeleg-store-pagefault/mspf_trap.S -o src/medeleg-store-pagefault/mspf_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/medeleg-store-pagefault/mspf_main.c -o src/medeleg-store-pagefault/mspf_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o medeleg-store-pagefault.elf src/boot.o src/uart.o src/medeleg-store-pagefault/mspf_trap.o src/medeleg-store-pagefault/mspf_main.o
```

Zero warnings. (`src/boot.o` and `src/uart.o` were built by the
same flags in an earlier `make` and were up to date; the link line
confirms `boot.o` is first.)

## Run output (QEMU 8.2.2, `qemu-system-riscv64 -machine virt -nographic -bios none -kernel medeleg-store-pagefault.elf`)

From `bench-logs/run1.log` (runs 1-3 byte-identical, all exit 0):

```
medeleg-store-pagefault: medeleg bit-15 store-page-fault trap destination switch
m-mode: pmpcfg0=0x1f (entry0: NAPOT all R|W|X)
m-mode: satp=0x8000000000080005 (MODE=8 ASID=0)
m-mode: phase-A medeleg write=0x8000 readback=0x8000
m-mode: entering S-mode (phase A, bit 15 set)
medeleg-store-pagefault: store-page-fault trap destination switch test
boot: medeleg=0x0
table: root[2]->l1[0]->l0 identity [0x80000000,0x80080000) R|W|X, l0[128]=0 (fault page 0x80080000)
phaseA: medeleg bit15 set readback=0x8000
phaseB: medeleg bit15 clear readback=0x0
pA: s_traps=1 scause=0xf sepc=0x80000300 expected=0x80000300 stval=0x80080000 sstatus_spp=1
pA: m_traps=1 mcause=0x9 mepc=0x80000304
pB: m_traps=2 mcause=0xf mepc=0x80000394 expected=0x80000394 mtval=0x80080000 mstatus_mpp=1 s_traps=1
quiet: m_traps=2 s_traps=1
checksum=0x5f72e2349de7d1fa
RESULT: PASS (checks=34)
```

## What the numbers mean

- `satp=0x8000000000080005`: MODE=8 (Sv39), ASID=0, root PPN
  matches the hand-built table page; `pmpcfg0=0x1f` confirms the
  NAPOT R|W|X grant the S-mode phase needs.
- Phase A (bit 15 set): exactly one S-mode trap, `scause=0xf`,
  `sepc=0x80000300` exactly at the faulting store instruction
  while `stval=0x80080000` is the faulting data address; the two
  differ, which is what separates a store page fault from an
  instruction page fault (where both equal the fetch address).
  The trap arrived from S-mode (`sstatus.SPP=1`). The M-mode
  handler saw exactly one trap and it was the phase-A return
  `ecall` (`mcause=0x9`): the store fault did not leak to M-mode.
- Phase B (bit 15 clear): exactly one further M-mode trap,
  `mcause=0xf`, `mepc=0x80000394` exactly at the faulting store
  instruction, `mtval=0x80080000`, arriving from S-mode
  (`mstatus.MPP=1`); the S-mode trap count stayed 1.
- The 2,000,000-iteration quiet window moved neither counter.
- FNV-1a `0x5f72e2349de7d1fa` over the ten verdict values
  (per-mode counts, causes, PCs, trap values, both `medeleg`
  readbacks) is byte-identical across all three runs.

## Verification record

- 34 checks, 0 failures, verdict PASS, QEMU exit code 0 on all
  three runs; `diff` of the three raw logs is empty.
- Every PTE the walk depends on was verified by hand before
  `satp` was written: both pointer entries are V-only with the
  exact table PPNs, leaf 0 carries V|R|W|X with PPN `0x80000`,
  leaf 127 is valid, and `l0_id[128]` reads back zero.
- The faulting store address in each phase was taken with a
  numeric assembler local label (`la t1, 0f`) at the faulting
  instruction itself, so `sepc`/`mepc` are checked against the
  exact instruction address rather than a guessed constant.
- Toolchain: riscv64-unknown-elf-gcc 13.2.0
  (`~/workspace/toolchains/ubuntu-rv64`), QEMU 8.2.2
  (`~/workspace/qemu`), `-bios none`, single hart.
