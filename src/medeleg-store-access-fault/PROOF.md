<!-- PROOF-HEADER
Checks: 41
Mismatches: 0
Checksum: 0x943ff096d0326162
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: medeleg bit-7 store-access-fault trap destination switch (backlog item "riscv medeleg-store-access-fault")

## What was built

`src/medeleg-store-access-fault/`: a bare-metal RISC-V program that
executes the same S-mode store to a PMP-denied page in two phases
and checks the trap destination flips with `medeleg` bit 7. The
leaf PTE over the store page is VALID with R|W, so address
translation succeeds and the fault comes from a locked PMP TOR
deny entry covering exactly the store page: a PMP deny raises an
access fault (`mcause = 7`), never a page fault, because PMP is
checked on the physical address after the page walk succeeds.
Sibling of the done module `src/medeleg-store-pagefault` (bit 15,
invalid PTE): same M->S drop, same Sv39 identity-map build, same
ecall return path, same FNV-1a checksum and virt test-device
finisher pattern.

M-mode builds a minimal Sv39 page table by hand: `root[2]` ->
`l1_id[0]` -> `l0_id[]`, identity-mapping `[0x80000000, 0x80080000)`
with R|W|X, plus `l0_id[128]` valid with R|W (no X) over the store
page at `0x80080000`. The PMP layout is three TOR entries,
lowest-numbered match wins: entry 0 = TOR allow
`[0, 0x80080000)` R|W|X unlocked (`pmpaddr0 = 0x20020000`), entry 1
= LOCKED TOR deny `[0x80080000, 0x80081000)` (`pmpaddr1 =
0x20020400`, no permissions), entry 2 = TOR allow `[0x80081000,
0x100000000)` R|W|X unlocked (`pmpaddr2 = 0x40000000`),
`pmpcfg0 = 0x000f880f`. Every `pmpcfg`/`pmpaddr` register is read
back and verified; Sv39 is enabled via `satp` + `sfence.vma`
before the drop to S-mode. M-mode is never translated and never
touches the denied page, so the locked entry only bites the
S-mode store.

Phase A runs with `medeleg` bit 7 set (readback `0x80` on QEMU
8.2.2): the S-mode store to the denied page must trap in S-mode
with `scause = 0x7` (store/AMO access fault), `sepc` at the
faulting store instruction, and `stval` holding the faulting data
address, while the M-mode handler sees only the phase-A return
`ecall`. Phase B runs with bit 7 clear (readback `0x0`): the same
store must trap in M-mode with `mcause = 0x7`, `mepc` at the
faulting store, and `mtval` at the store address, with the S-mode
trap count unchanged. Each phase first stores to an allowed
scratch word (control, completes with no trap, proving the fault
comes from the PMP entry). A quiet spin window moves neither
trap counter, and the boot `medeleg` is restored and reported at
the end. Five files, sharing only `src/boot.S` and `src/uart.c`
with the other demos. Exactly one mechanism is under test: the
destination of a supervisor-mode store-access-fault trap as
`medeleg` bit 7 flips.

- `msaf_trap.S`: M-mode trap entry (records `mcause`/`mepc`/
  `mstatus`/`mtval` in `m_regs`; the only M-mode traps possible
  are the phase-A `ecall` and the phase-B access fault, so the
  handler loads the armed continuation from its save area into
  `mepc`, sets `mstatus.MPP` to M-mode, and `mret`s into it) and
  S-mode trap entry (records `scause`/`sepc`/`sstatus`/`stval` in
  `s_regs`, resumes at the address the payload stored, and
  `sret`s back). Zero continuation or zero resume address parks
  the hart.
- `msaf_main.c`: two-phase driver. Programs the three PMP TOR
  entries and verifies every readback, builds the page tables
  and verifies every PTE by hand, writes `satp`, verifies the
  `medeleg` readbacks, drops M -> S via `sret`, stores to the
  denied page from S-mode in both phases, restores the boot
  `medeleg`, publishes every measured value, runs the checks,
  computes an FNV-1a checksum over the verdict values, and
  reports PASS/FAIL. On PASS it writes `0x5555` to the virt
  test-device finisher so the QEMU process exits 0; on FAIL it
  parks the hart.

## Build log

From `bench-logs/build.log` (riscv64-unknown-elf-gcc 13.2.0,
`-O2`, `boot.o` first in link order):

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/medeleg-store-access-fault/msaf_trap.S -o src/medeleg-store-access-fault/msaf_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/medeleg-store-access-fault/msaf_main.c -o src/medeleg-store-access-fault/msaf_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o medeleg-store-access-fault.elf src/boot.o src/uart.o src/medeleg-store-access-fault/msaf_trap.o src/medeleg-store-access-fault/msaf_main.o
```

Zero warnings. (`src/boot.o` and `src/uart.o` were built by the
same flags in an earlier `make` and were up to date; the link line
confirms `boot.o` is first.)

## Run output (QEMU 8.2.2, `qemu-system-riscv64 -machine virt -nographic -bios none -kernel medeleg-store-access-fault.elf`)

From `bench-logs/run1.log` (runs 1-3 byte-identical, all exit 0):

```
medeleg-store-access-fault: medeleg bit-7 store-access-fault trap destination switch
m-mode: pmpcfg0=0xf880f pmpaddr0=0x20020000 pmpaddr1=0x20020400 pmpaddr2=0x40000000
m-mode: satp=0x8000000000080005 (MODE=8 ASID=0)
m-mode: phase-A medeleg write=0x80 readback=0x80
m-mode: entering S-mode (phase A, bit 7 set)
medeleg-store-access-fault: store-access-fault trap destination switch test
boot: medeleg=0x0 restored readback=0x0
table: root[2]->l1[0]->l0 identity [0x80000000,0x80080000) R|W|X, l0[128] valid R|W (PMP-denied store page 0x80080000)
pmp: cfg0=0xf880f addr0=0x20020000 addr1=0x20020400 addr2=0x40000000
phaseA: medeleg bit7 set readback=0x80
phaseB: medeleg bit7 clear readback=0x0
pA: s_traps=1 scause=0x7 sepc=0x80000312 expected=0x80000312 stval=0x80080000 sstatus_spp=1
pA: m_traps=1 mcause=0x9 mepc=0x80000316
pB: m_traps=2 mcause=0x7 mepc=0x800003b6 expected=0x800003b6 mtval=0x80080000 mstatus_mpp=1 s_traps=1
quiet: m_traps=2 s_traps=1
checksum=0x943ff096d0326162
RESULT: PASS (checks=41)
```

## What the numbers mean

- `pmpcfg0=0xf880f` (entry 0 TOR R|W|X unlocked, entry 1 locked
  TOR deny, entry 2 TOR R|W|X unlocked) and the three `pmpaddr`
  readbacks `0x20020000`/`0x20020400`/`0x40000000` are exactly the
  written values, so the allow/deny/allow triple the fault depends
  on is the configuration the faults actually ran under.
- `satp=0x8000000000080005`: MODE=8 (Sv39), ASID=0, root PPN
  matches the hand-built table page.
- Phase A (bit 7 set): exactly one S-mode trap, `scause=0x7`,
  `sepc=0x80000312` exactly at the faulting store instruction
  while `stval=0x80080000` is the faulting data address; the two
  differ, which is what separates a store access fault from an
  instruction access fault (where both equal the fetch address).
  The trap arrived from S-mode (`sstatus.SPP=1`). The M-mode
  handler saw exactly one trap and it was the phase-A return
  `ecall` (`mcause=0x9`): the store fault did not leak to M-mode.
- Phase B (bit 7 clear): exactly one further M-mode trap,
  `mcause=0x7`, `mepc=0x800003b6` exactly at the faulting store
  instruction, `mtval=0x80080000`, arriving from S-mode
  (`mstatus.MPP=1`); the S-mode trap count stayed 1.
- The control store to the allowed scratch word in each phase
  completed with no trap (`s_traps` exactly 1 in both phases),
  which proves the fault comes from the PMP deny entry and not
  from the store path itself.
- The 2,000,000-iteration quiet window moved neither counter.
- FNV-1a `0x943ff096d0326162` over the fourteen verdict values
  (per-mode counts, causes, PCs, trap values, both `medeleg`
  readbacks, `pmpcfg0` and all three `pmpaddr` readbacks) is
  byte-identical across all three runs.

## Verification record

- 41 checks, 0 failures, verdict PASS, QEMU exit code 0 on all
  three runs; `cmp` of the three raw logs is empty.
- Every PTE the walk depends on was verified by hand before
  `satp` was written: both pointer entries are V-only with the
  exact table PPNs, leaf 0 carries V|R|W|X with PPN `0x80000`,
  leaf 127 is valid, and `l0_id[128]` reads back valid R|W (no X)
  with the exact store-page PPN, so translation succeeds and the
  only possible fault source is the PMP deny.
- The faulting store address in each phase was taken with a
  numeric assembler local label (`la t1, 0f`) at the faulting
  instruction itself, so `sepc`/`mepc` are checked against the
  exact instruction address rather than a guessed constant. The
  handlers never advance the PC past the faulting store, so no
  instruction-length arithmetic exists to get wrong.
- Toolchain: riscv64-unknown-elf-gcc 13.2.0
  (`~/workspace/toolchains/ubuntu-rv64`), QEMU 8.2.2
  (`~/workspace/qemu`), `-bios none`, single hart.
