<!-- PROOF-HEADER
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: Sv39 page-table walk and fault path (backlog item 39)

## What was built

`src/sv39/`: a bare-metal RISC-V program that constructs a minimal Sv39
page table by hand in RAM, enables it with `satp` (MODE=8, ASID=0)
followed by `sfence.vma`, drops from M-mode to S-mode, and exercises
the hardware page-table walker from S-mode. Three files, sharing only
`src/boot.S` and `src/uart.c` with the other demos.

- `sv39_main.c`: UART bring-up, control read/write of the data page
  before translation, hand-built page tables with readback checks of
  every PTE, PMP grant for the S-mode phase, `satp` programming with
  MODE/ASID readback checks, the `mret` into S-mode, and the S-mode
  test phase (mapped round trip plus three fault tests) with
  in-program PASS/FAIL checks.
- `sv39_trap.S`: minimal M-mode trap entry. `mscratch` points at the
  `sv39_save` array; the handler swaps `t0`, records
  `mcause`/`mepc`/`mtval`/`stval`, loads the resume PC the test stored
  beforehand, sets `mepc` to it, flags the trap as seen, restores the
  registers, and returns with `mret`. Resume addresses come from
  in-assembly numeric local labels (`la t0, 1f`); no C `&&label`.
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make sv39.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel sv39.elf`
(or `make run-sv39`).

## Configuration under test

- QEMU 8.2.2, `virt` machine, single hart, `mhartid = 0`. All numbers
  below are measured on QEMU, not on silicon.
- Toolchain: xPack GNU RISC-V Embedded GCC 15.2.0, `-O2`
  `-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`.
- The program boots in M-mode (QEMU loads the ELF straight into M-mode
  with `-bios none`). M-mode builds the tables, writes `satp`, issues
  `sfence.vma`, installs the M-mode trap vector, then drops to S-mode
  with `mret` (MPP=S). Every translated access happens in S-mode; all
  traps are taken in M-mode, so the fault state lands in the `m*`
  CSRs.

## The table (hand-built, verified by readback)

Sv39 PTE format (RISC-V privileged spec): bits 53:10 are the physical
page number, bits 9:0 are flags `D A G U X W R V` (bit 7 = D ... bit 0
= V). A PTE with V=1 and none of R/W/X is a pointer to the next table
level; V=1 with any of R/W/X is a leaf. A 4 KiB leaf is only valid in a
level-0 table (a leaf one level up would be a 2 MiB superpage); a
level-2 leaf covers 1 GiB.

Measured PTEs, identical on all three runs (addresses shift only if
the sources change):

| entry | value | PPN | flags | meaning |
|---|---|---|---|---|
| `root_pt[1]` | `0x20001c01` | `0x80007` | `V` | pointer to the test level-1 table |
| `l1_test[0]` | `0x20001801` | `0x80006` | `V` | pointer to the test level-0 table |
| `l0_test[0]` | `0x20000cc7` | `0x80003` | `V R W A D` | 4 KiB leaf: the one mapped data page |
| `root_pt[2]` | `0x200000cf` | `0x80000` | `V R W X A D` | 1 GiB megapage leaf, identity `[0x80000000, 0xC0000000)` |
| `root_pt[0]` | `0x20001401` | `0x80005` | `V` | pointer to the UART level-1 table |
| `l1_uart[128]` | `0x20001001` | `0x80004` | `V` | pointer to the UART level-0 table |
| `l0_uart[0]` | `0x40000c7` | `0x10000` | `V R W A D` | 4 KiB leaf: UART at `0x10000000` |

All other entries in all five table pages are zero (invalid), which is
what the fault tests rely on. The `root_pt[2]` megapage and the UART
chain are scaffolding the S-mode phase needs (its code, stack, tables,
and a way to print); the mechanism under test is the
`root[1] -> l1_test[0] -> l0_test[0]` chain mapping VA `0x40000000`
(VPN[2]=1, VPN[1]=0, VPN[0]=0) to the data page. M-mode readback checks
assert every pointer PTE is V-only, the leaf has V/R/W, and each PPN
equals the page number of the intended next-level table or page.

## PMP grant (required scaffolding, measured)

With PMP implemented, the privileged spec denies an S-mode access that
matches no PMP entry, and QEMU enforces this. Without a grant, the
first S-mode instruction fetch faults (verified during development:
`fault_fetch`, `mcause=1`, before any test ran). M-mode therefore
programs one PMP entry before dropping privilege:

- `pmpaddr0 = 0`, `pmpaddr1 = 0x20100000`, `pmpcfg0 = 0x0F00`:
  entry 1 is TOR `[0, 0x80400000)` with R/W/X. It covers the
  identity-mapped code/data/stack, the page tables, and the UART page.
  Readback check asserts `pmpcfg0 == 0x0F00`.
- Unlocked PMP entries are not checked for M-mode, so the M-mode phase
  is unaffected.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

| check | run1 | run2 | run3 |
|---|---|---|---|
| data page R/W before translation (control) | ok | ok | ok |
| `satp` | `0x8000000000080008` (MODE=8 ASID=0) | same | same |
| mapped: stored pattern, loaded back, no trap | `0xdeadbeefcafebabe`, seen=0 | same | same |
| physical page holds pattern after mapped store | `0xdeadbeefcafebabe` | same | same |
| fault-l0 (VA `0x40001000`, bad level-0 PTE): `mcause` | 13 | 13 | 13 |
| fault-l0: `mtval` | `0x40001000` | `0x40001000` | `0x40001000` |
| fault-l0: `stval` | `0x0` | `0x0` | `0x0` |
| fault-l0: `mepc` == resume-4 | `0x80000360` | `0x80000360` | `0x80000360` |
| fault-l1 (VA `0x50000000`, bad level-1 PTE): `mcause` | 13 | 13 | 13 |
| fault-l1: `mtval` | `0x50000000` | `0x50000000` | `0x50000000` |
| fault-l1: `stval` | `0x0` | `0x0` | `0x0` |
| fault-l1: `mepc` == resume-4 | `0x80000360` | `0x80000360` | `0x80000360` |
| fault-l2 (VA `0xC0000000`, bad root PTE): `mcause` | 13 | 13 | 13 |
| fault-l2: `mtval` | `0xC0000000` | `0xC0000000` | `0xC0000000` |
| fault-l2: `stval` | `0x0` | `0x0` | `0x0` |
| fault-l2: `mepc` == resume-4 | `0x80000360` | `0x80000360` | `0x80000360` |
| RESULT | PASS | PASS | PASS |

All three runs are logically identical; the only difference in the raw
logs is the host PID in QEMU's timeout shutdown line.

What each value means:

- `mapped ... seen=0`: the store and load through VA `0x40000000`
  completed with no trap, and the value read back equals the value
  written (`0xdeadbeefcafebabe`).
- `physical page holds 0xdeadbeefcafebabe`: the walk landed on the
  intended frame, confirmed by reading the data page directly by its
  physical address (S-mode identity mapping), so the translated store
  did not hit an alias.
- `mcause=13` on all three fault tests: load page fault, the
  architecturally correct cause for a walk that dies on an invalid PTE.
- `mtval` equals the faulting VA in each case: the walker reports the
  address that failed translation. `fault-l0` dies at the third lookup
  (`l0_test[1]` is zero), `fault-l1` at the second (`l1_test[128]` is
  zero), `fault-l2` at the first (`root_pt[3]` is zero).
- `mepc == resume-4`: the trap interrupted exactly the faulting `ld`.
  The faulting instruction is the 4-byte `ld t1,0(a0)` immediately
  before the resume label (verified in the disassembly: `auipc` at
  `0x8000035c`, `ld` at `0x80000360`, resume label at `0x80000364`).
- `stval=0x0`: these traps are taken in M-mode, so the hardware writes
  `mtval` only; `stval` reads back its reset value, proving the S-mode
  trap CSRs are untouched. This is the measured value, not an
  assumption.

## Full run-1 output (runs 2 and 3 are identical)

```
sv39-walk: Sv39 page-table walk and fault path (backlog item 39)
m-mode: mhartid=0
control: data page readable/writable before translation: ok
table: VA 0x40000000 -> vpn2=1 vpn1=0 vpn0=0
table: root_pt[1] =0x20001c01 ppn=0x80007 flags=V
table: l1_test[0] =0x20001801 ppn=0x80006 flags=V
table: l0_test[0] =0x20000cc7 ppn=0x80003 flags=VRWAD
table: root_pt[2] =0x200000cf ppn=0x80000 flags=VRWXAD
table: root_pt[0] =0x20001401 ppn=0x80005 flags=V
table: l1_uart[128]=0x20001001 ppn=0x80004 flags=V
table: l0_uart[0] =0x40000c7 ppn=0x10000 flags=VRWAD
m-mode: pmpcfg0=0xf00 (entry1: TOR [0, 0x80400000) R|W|X)
m-mode: satp=0x8000000000080008 (MODE=8 ASID=0)
m-mode: entering S-mode
s-mode: entered, translation active
mapped: stored 0xdeadbeefcafebabe via VA 0x40000000, loaded 0xdeadbeefcafebabe seen=0
mapped: physical page now holds 0xdeadbeefcafebabe
fault-l0: seen=1 mcause=0xd mepc=0x80000360 mtval=0x40001000 stval=0x0 resume-4=0x80000360
fault-l1: seen=1 mcause=0xd mepc=0x80000360 mtval=0x50000000 stval=0x0 resume-4=0x80000360
fault-l2: seen=1 mcause=0xd mepc=0x80000360 mtval=0xc0000000 stval=0x0 resume-4=0x80000360
RESULT: PASS (mapped round-trip ok, all faults mcause=13)
done
```

(`qemu-system-riscv64: terminating on signal 15` follows: the program
parks the hart in `wfi` after printing `done`, and the `timeout 15`
used for the run logs terminates QEMU. Exit 124 is the expected
timeout, not a failure.)

## QEMU model behavior this run depends on (read before porting)

Verified against the QEMU 8.2.2 source, not assumed:

- S-mode (and U-mode) accesses that match no PMP entry are denied
  (`pmp_hart_has_privs_default` returns false for non-M modes when PMP
  is implemented). This matches the privileged spec, and the demo would
  not reach its first S-mode instruction without the PMP grant above.
- A 4 KiB leaf PTE placed at level 1 is rejected by the walker with a
  misaligned-PPN page fault (`ppn & ((1 << ptshift) - 1)` at
  `leaf:`), because a level-1 leaf is a 2 MiB superpage. The first
  version of this demo made exactly that mistake and measured a store
  page fault (`mcause=15`, `mtval=0x40000000`) on the mapped store;
  the three-level table above is the corrected construction.
- `satp` writes are gated on `mstatus.TVM` only when attempted from
  S-mode; from M-mode the write takes effect immediately, and
  `sfence.vma` orders the subsequent walks.

## Limits of verification

- All measurements are from QEMU 8.2.2's `virt` machine, not silicon.
  Cycle counts are not reported because QEMU's `rdcycle` is host-time
  driven; the claims here are about values (PTE bits, `mcause`,
  `mtval`, `mepc`), which the emulator models faithfully.
- The A/D-bit update path is not exercised: the leaf PTEs are built
  with A and D preset, so no hardware update is needed.
- `sfence.vma` is issued once after the `satp` write; TLB shootdown
  across harts is not tested (single hart).

Build log: `bench-logs/build.log`. Full run outputs:
`bench-logs/run1.log`, `bench-logs/run2.log`, `bench-logs/run3.log`.
