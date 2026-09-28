<!-- PROOF-HEADER
Checks: 60
Mismatches: 0
Checksum: 0x69d07a40571b29d3
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: sfence.vma rs1-scoped invalidation (backlog item "riscv sfence-vma-rs1-scoped")

## What was built

`src/sfence-vma-rs1-scoped/`: a bare-metal RISC-V program that
remaps two virtual addresses between two pairs of physical pages
and reads the words back in three S-mode phases, fencing only
the first address. M-mode builds a minimal Sv39 page table by
hand: `root[2]` -> `l1_id[0]` -> `l0_id[]` identity-maps the
program window `[0x80000000, 0x80080000)` with R|W|X,
`root[1]` -> `l1_t1[0]` -> `l0_t1[0]` maps `VA1 = 0x40000000`
to the canary-A page, and `root[3]` -> `l1_t2[0]` -> `l0_t2[0]`
maps `VA2 = 0xc0000000` to the canary-C page. One PMP NAPOT
entry grants S-mode R|W|X over the whole address space; Sv39 is
enabled via `satp` + `sfence.vma` before the drop to S-mode. The
A/D bits are pre-set on every leaf so no access-fault trap fires
to update them (`menvcfg.ADUE` is 0 on this QEMU). Exactly one
mechanism is under test: whether `sfence.vma` with `rs1` set
invalidates only the named address.

- `srs_trap.S`: M-mode trap entry (records `mcause`/`mepc`/
  `mstatus`/`mtval` in `m_regs`; the only M-mode traps possible
  are the three phase-return `ecall`s, so the handler loads the
  armed continuation from its save area into `mepc`, sets
  `mstatus.MPP` to M-mode, and `mret`s). The S-mode entry counts
  and parks: no S-mode trap is expected at any point, so any
  arrival is a loud failure.
- `srs_main.c`: setup, the three S-mode payloads, the two M-mode
  mid phases, and the report.

## Sequence

Phase A runs in S-mode with VA1 pointing at the canary-A page
and VA2 at the canary-C page: the loads must return canaries A
and C, populating the hart's translation cache with both
mappings. The payload `ecall`s back to M-mode. M-mode
(`mid_to_B`) snapshots the trap record, rewrites both leaf PTEs
(VA1 to the canary-B page, VA2 to the canary-D page), issues
**no global fence**, then executes `sfence.vma` with
`rs1 = VA1` only (the disassembly shows `lui a5, 0x40000`
immediately before `sfence.vma a5`, i.e. nonzero `rs1`, zero
`rs2`). Phase B runs in S-mode and records both readbacks. The
payload `ecall`s back. M-mode (`mid_to_C`) snapshots the trap
record and executes a global `sfence.vma`. Phase C runs in
S-mode: the loads must return canary B and fresh canary D. The
report snapshots the final trap record, restores `satp` to Bare
and the PMP entry to 0, prints every measured value, runs the
checks, takes a 2M-iteration quiet window, and prints the
verdict.

## Results (QEMU 8.2.2, 3 runs byte-identical, md5 1da808a7f3d711615321257fe8c5e9cf)

- `satp` readback `0x800000000008000d` (MODE=8 Sv39, ASID=0,
  root PPN correct); `pte1_before = 0x200018c7`
  (VA1 -> PA1), `pte2_before = 0x200010c7` (VA2 -> PA3),
  `pte1_after = 0x200014c7` (VA1 -> PA2),
  `pte2_after = 0x20000cc7` (VA2 -> PA4): both rewrites landed
  in memory before the scoped fence.
- Phase A readbacks `0xaaaaaaaaaaaaaaaa` (A) and
  `0xcccccccccccccccc` (C): both translations cached.
- Phase B readbacks `0xbbbbbbbbbbbbbbbb` (fresh B at VA1) and
  `0xdddddddddddddddd` (fresh D at VA2): the rs1-scoped fence
  did **not** scope the invalidation on this implementation.
- Phase C readbacks `0xbbbbbbbbbbbbbbbb` (B) and
  `0xdddddddddddddddd` (fresh D): the global fence is a no-op
  confirmation.
- `m_traps = 3`, each with `mcause = 0x9` and `mepc` exactly at
  its phase's `ecall` site; `s_traps = 0`; quiet window moved
  neither counter.
- 60 checks, 0 mismatches, FNV-1a `0x69d07a40571b29d3`
  byte-identical across 3 QEMU 8.2.2 runs, QEMU exit 0 on all
  three, Verdict PASS.

## Why this is the real mechanism

The backlog premise was that phase B would read stale canary C
at VA2 because no fence named it. The first build asserted
exactly that and failed honestly (58 of 60 checks passed; the
two failures were the VA2 staleness checks). The failure could
not be a test bug: the disassembly confirms the fence named
only VA1 (`sfence.vma a5` with `a5 = 0x40000000`, `rs2 = x0`),
the PTE readbacks confirm both rewrites landed in memory before
the fence, and the sibling module `src/sfence-vma-remap`
(failing no-fence control, shipped 2026-09-27) proves the same
harness reads stale when no fence is issued at all. The
conclusion is measured, not assumed: on QEMU 8.2.2,
`sfence.vma` with `rs1` set behaves as a full invalidation,
matching the independent observation in the Grinch OS project
that QEMU treats any `sfence.vma` as a full local TLB flush
(lfd/grinch commit `ccccd68`). Over-invalidation is still
correct; a guest relying on scoping for performance simply
cannot get it on this implementation.

## Build and run

```
make sfence-vma-rs1-scoped.elf
make run-sfence-vma-rs1-scoped QEMU=~/workspace/qemu/usr/bin/qemu-system-riscv64
```

Toolchain: Ubuntu `gcc-riscv64-unknown-elf` 13.2.0
(`~/workspace/toolchains/ubuntu-rv64`). PASS writes `0x5555` to
the virt test-device finisher, so QEMU exits 0; on FAIL the hart
parks.
