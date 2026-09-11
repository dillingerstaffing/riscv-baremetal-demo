<!-- PROOF-HEADER
Checks: 31
Mismatches: 0
Checksum: 0x196010adc8b6fa27
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: sstatus.SUM as the S-mode gate on U-page access (proof-backlog item "sstatus-sum-probe")

## What was built

`src/sstatus-sum/`: a bare-metal RISC-V program that verifies the
gate behavior of the `sstatus.SUM` bit on the QEMU `virt` board.
Three files, about 700 lines total, sharing only `src/boot.S` and
the UART driver with the other demos via the Makefile. Exactly one
mechanism is under test: `sstatus.SUM` (bit 18) controls whether
S-mode may access pages marked U=1 (privileged spec section
4.1.1.8: "when SUM=0, S-mode memory accesses to pages that are
accessible by U-mode (U=1 in pte) will fault"). With SUM=0 a
supervisor load from a U-page must raise a load page fault; with
SUM=1 the same load must succeed and return the page contents.

- `ssum_trap.S`: S-mode trap entry. Swaps t0 with sscratch, saves
  every general-purpose register, then calls the C handler
  `ssum_trap_handler()` on a dedicated trap stack. The handler
  takes the one expected load page fault, records
  scause/stval/sepc, advances the saved sepc by 4 to skip the
  faulting 4-byte ld (t0/t1 are not compressible registers, so the
  ld is always exactly 4 bytes), and counts the trap; any further
  trap is counted as unexpected. Exit restores every register and
  srets at the advanced sepc. The M-mode vector `m_trap_entry` is a
  minimal record-and-park handler; no M-mode trap is expected after
  boot, and one would show up in the log as a FAIL rather than a
  silent hang.
- `ssum_main.c`: M-mode boot (store the canary word in the U page
  as a physical control write, build the Sv39 tables by hand, open
  one PMP NAPOT entry, delegate the load page fault via
  `medeleg` bit 13, install direct-mode `stvec`, write
  `satp` MODE=Sv39 and `sfence.vma`, `mret` into S-mode with
  MPP=01) and the S-mode two-phase payload: phase A with SUM
  explicitly clear does one ld from the U VA 0x40000000 and
  requires exactly one trap with scause 0xd, stval = the faulting
  VA, sepc = the labeled faulting load; the handler advances sepc
  by 4 and srets, and a t0 poisoned to 0 before the ld proves the
  load never completed. Phase B sets SUM via `csrs sstatus` and
  requires the same ld to return the canary with no new trap. A
  failed check prints `FAIL` and flips the verdict; `RESULT: PASS`
  is printed only when every check held. On PASS the machine is
  shut down via the virt test-device finisher so the QEMU process
  exits 0; on FAIL the hart parks.
- `PROOF.md` (this file), plus an empty `bench-logs/` placeholder
  matching the convention of the other modules.

Build: `make sstatus-sum.elf` (added to `all` in the Makefile).
Run: `timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel sstatus-sum.elf`
(or `make run-sstatus-sum`).

## Configuration under test

- Hart: mhartid = 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`; the module then drops to S-mode itself.
- Sv39 page tables built by hand in M-mode before `satp`:
  root[2] is a 1 GiB megapage leaf identity-mapping
  [0x80000000, 0xC0000000) (code, data, stack, the tables; V|R|W|X,
  supervisor); root[0] is a 1 GiB megapage leaf identity-mapping
  [0, 0x40000000), which carries the UART MMIO page at 0x10000000
  (V|R|W|X, supervisor); root[1] -> l1_test[0] -> l0_test[0] is the
  three-level chain for the test VA, ending in a 4 KiB leaf mapping
  VA 0x40000000 to the canary page with V|R|U|A|D (no W, no X).
  Every other entry in all three tables is zero (invalid).
- `medeleg = 0x2000` (only bit 13, load page fault): the phase-A
  fault is taken in S-mode, so hardware writes stval with the
  faulting VA.
- One PMP NAPOT entry covering the whole address space with R|W|X,
  granting S-mode access before the drop (lower modes default-deny
  with no matching entry). Unlocked entries are not checked for
  M-mode, so the M-mode phase is unaffected.
- Interrupts are never enabled (`sie` = 0, `sstatus.SIE` = 0), so
  the only trap in the run is the synchronous phase-A load page
  fault.

## Sequence and controls

1. Control: M-mode stores the canary word 0xc0decafe5ca1ab1e in the
   U page as a plain physical write before any table exists and
   reads it back, proving the page is good RAM.
2. Table checks: every table page is 4 KiB aligned; the pointer
   entries are pure V-only pointers with PPNs matching the table
   page numbers; the U leaf has exactly V|R|U (plus A|D, which
   QEMU would otherwise update on the walk) and neither W nor X.
   `pmpcfg0` reads back 0x1f, `medeleg` bit 13 sticks, `stvec` is
   direct mode, and `satp` reads back MODE=8 with the root PPN.
3. Phase A, gate closed: `sstatus.SUM` explicitly cleared (read
   back as 0). One `ld t0, 0(t1)` from VA 0x40000000 at the global
   label `sums_fault_load`; t0 is poisoned to 0 immediately before
   it. The delegated S-mode handler must record scause = 0xd
   (load page fault), stval = 0x40000000, sepc = the label
   address, advance sepc by 4, and sret. Afterward the trap
   counter must read 1, the stored load result must still be 0
   (the ld never completed), and the resume marker must be 1
   (execution continued via sret, not a hang).
4. Phase B, gate open: `csrs sstatus, SUM` (read back as 1). The
   same `ld` from the same VA must return the canary
   0xc0decafe5ca1ab1e, and the trap counter must still read 1:
   no new trap fired.

## Results (three QEMU runs)

Three runs were performed after the final build; all print
`RESULT: PASS` with `checks=31 fails=0`, and all three are fully
byte-identical UART output. The table below is from the final
build; every value is a measured register read or counter, never a
computation from an assumption.

| step | run1 | run2 | run3 |
|---|---|---|---|
| canary control write/readback | ok | ok | ok |
| `l0_test[0]` leaf flags | VRUAD | VRUAD | VRUAD |
| `pmpcfg0` | 0x1f | 0x1f | 0x1f |
| `medeleg` | 0x2000 | 0x2000 | 0x2000 |
| `satp` (MODE=8, root PPN) | 0x8000000000080009 | 0x8000000000080009 | 0x8000000000080009 |
| `sstatus.SUM` at phase A (expect 0) | 0 | 0 | 0 |
| phase-A traps (expect 1) | 1 | 1 | 1 |
| trap scause (expect 0xd) | 0xd | 0xd | 0xd |
| trap stval (expect 0x40000000) | 0x40000000 | 0x40000000 | 0x40000000 |
| trap sepc (expect = fault load addr) | 0x800004f8 | 0x800004f8 | 0x800004f8 |
| `sums_fault_load` address | 0x800004f8 | 0x800004f8 | 0x800004f8 |
| phase-A load result (expect 0) | 0x0 | 0x0 | 0x0 |
| phase-A resumed marker (expect 1) | 1 | 1 | 1 |
| `sstatus.SUM` at phase B (expect 1) | 1 | 1 | 1 |
| phase-B canary readback (expect 0xc0decafe5ca1ab1e) | 0xc0decafe5ca1ab1e | 0xc0decafe5ca1ab1e | 0xc0decafe5ca1ab1e |
| traps after phase B (expect 1) | 1 | 1 | 1 |
| checks / mismatches | 31 / 0 | 31 / 0 | 31 / 0 |
| FNV-1a checksum of verdict values | 0x196010adc8b6fa27 | 0x196010adc8b6fa27 | 0x196010adc8b6fa27 |
| RESULT | PASS | PASS | PASS |

What each value means:

- `scause = 0xd` with `stval = 0x40000000` and
  `sepc = sums_fault_load`: with SUM=0, the supervisor load from
  the U=1 leaf raised exactly the load page fault the spec
  mandates, with the faulting VA in stval (the trap was delegated
  to S-mode, so hardware wrote stval, not mtval) and sepc pointing
  at the faulting `ld`.
- `load_result = 0x0` after phase A: t0 was poisoned to 0 right
  before the faulting ld, and the handler's sepc+4 skipped the ld,
  so the post-trap store wrote the poison, not the canary. Had the
  ld completed, the result would have been the canary; 0 proves the
  load truly faulted rather than returning data.
- `resumed = 1`: execution continued after the trap via sret at
  sepc+4, landing on the resume-marker store; the run did not
  park, spin, or take a second trap.
- Phase-B `load_result = 0xc0decafe5ca1ab1e` with the trap
  counter still at 1: after `csrs sstatus, SUM`, the identical
  load from the identical VA returned the canary word with no
  trap. The only change between the faulting load and the
  succeeding load is the SUM bit, so SUM is what gates the access.
- The checksum is 64-bit FNV-1a over the deterministic measured
  verdict values (the trap's scause/stval/sepc, the trap count,
  the bad-trap flag, the SUM readbacks in both phases, the
  phase-A load result, the phase-B canary readback, and the resume
  marker). It is identical across runs because every measured
  value is identical; nothing host-timing dependent enters it.

## Build log

The genuine build log of the final build (gcc 13.2.0, no warnings):

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/boot.S -o src/boot.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/uart.c -o src/uart.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sstatus-sum/ssum_trap.S -o src/sstatus-sum/ssum_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sstatus-sum/ssum_main.c -o src/sstatus-sum/ssum_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o sstatus-sum.elf src/boot.o src/uart.o src/sstatus-sum/ssum_trap.o src/sstatus-sum/ssum_main.o
```

Run logs: three runs were captured in full (each exits 0 via the
virt test-device finisher on PASS). The runs are byte-identical;
run 1's output:

```
sstatus-sum: sstatus.SUM gate on U-page loads (proof-backlog item "sstatus-sum-probe")
control: canary 0xc0decafe5ca1ab1e stored in the U page: ok
table: VA 0x40000000 -> vpn2=1 vpn1=0 vpn0=0
table: root_pt[2] =0x200000cf ppn=0x80000 flags=VRWXAD
table: root_pt[1] =0x20002001 ppn=0x80008 flags=V
table: root_pt[0] =0xcf ppn=0x0 flags=VRWXAD
table: l1_test[0] =0x20001c01 ppn=0x80007 flags=V
table: l0_test[0] =0x200018d3 ppn=0x80006 flags=VRUAD
m-mode: pmpcfg0=0x1f (entry0: NAPOT all-space R|W|X)
m-mode: medeleg=0x2000 (bit 13: load page fault delegated)
m-mode: satp=0x8000000000080009 (MODE=8 ASID=0)
m-mode: entering S-mode
s-mode: entered, translation active
phase A: sstatus.SUM=0 (expect 0)
phase A: traps=1 (expect 1) scause=0xd (expect 0xd) stval=0x40000000 (expect 0x40000000) sepc=0x800004f8
fault load at sums_fault_load=0x800004f8 load_result=0x0 (expect 0) resumed=1 (expect 1)
phase B: sstatus.SUM=1 (expect 1)
phase B: load_result=0xc0decafe5ca1ab1e (expect canary 0xc0decafe5ca1ab1e) traps=1 (expect 1)
VERDICT phase_a_traps=1 phase_b_traps=0 scause=0xd stval=0x40000000 sepc=0x800004f8 canary_readback=0xc0decafe5ca1ab1e checksum=0x196010adc8b6fa27
checks=31 fails=0
RESULT: PASS
```

## Limits of verification (read before citing numbers)

- This verifies QEMU 8.2.2's Sv39 page-walk permission model and
  sstatus.SUM handling on the `virt` machine, not real silicon. The
  SUM gate behavior is architectural, but the observation is
  against the emulator.
- Only hart 0, only the supervisor load path, only direct S-mode
  trap entry. Store and instruction-fetch paths through U pages
  with SUM=0 are not exercised (they are the same permission bit
  in the walk, but this module measures loads only); the module is
  deliberately that small.
- The run is straight-line code with no polling windows: the
  phase-A trap count is exactly 1 by construction, and any extra
  trap would have been recorded by the handler and failed the
  run's checks.
- The A bit is pre-set in the U leaf PTE, so the walk never takes
  the hardware A-update path; the measured behavior is the
  permission check alone.

## Reproduction

```
make sstatus-sum.elf
timeout 30 qemu-system-riscv64 -machine virt -nographic -bios none -kernel sstatus-sum.elf
```

Toolchain used: `riscv64-unknown-elf-gcc` 13.2.0, QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`, Debian
1:8.2.2+ds-0ubuntu1.18).
