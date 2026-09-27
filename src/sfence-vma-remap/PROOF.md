<!-- PROOF-HEADER
Checks: 39
Mismatches: 0
Checksum: 0x421dacf7967027dc
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: sfence.vma remap visibility (backlog item "riscv sfence-vma-remap")

## What was built

`src/sfence-vma-remap/`: a bare-metal RISC-V program that remaps
one virtual address between two physical pages and reads the word
back in three S-mode phases. M-mode builds a minimal Sv39 page
table by hand: `root[2]` -> `l1_id[0]` -> `l0_id[]` identity-maps
the program window `[0x80000000, 0x80080000)` with R|W|X, and
`root[1]` -> `l1_test[0]` -> `l0_test[0]` is the single leaf under
test, mapping `TEST_VA = 0x40000000` to a page holding canary A
(`0xaaaaaaaaaaaaaaaa`). A second page holds canary B
(`0xbbbbbbbbbbbbbbbb`). One PMP NAPOT entry grants S-mode R|W|X
over the whole address space; Sv39 is enabled via `satp` +
`sfence.vma` before the drop to S-mode. The A/D bits are pre-set
on every leaf so no access-fault trap fires to update them
(`menvcfg.ADUE` is 0 on this QEMU). Exactly one mechanism is under
test: whether a PTE rewrite is visible to the hart before an
`sfence.vma` orders it.

- `svr_trap.S`: M-mode trap entry (records `mcause`/`mepc`/
  `mstatus`/`mtval` in `m_regs`; the only M-mode traps possible
  are the three phase-return `ecall`s, so the handler loads the
  armed continuation from its save area into `mepc`, sets
  `mstatus.MPP` to M-mode, and `mret`s). The S-mode entry counts
  and parks: no S-mode trap is expected at any point, so any
  arrival is a loud failure.
- `svr_main.c`: setup, the three S-mode payloads, the two M-mode
  mid phases, and the report.

## Sequence

Phase A runs in S-mode with the leaf pointing at PA1: the load
must return canary A, and the read populates the hart's
translation cache with the `TEST_VA` -> PA1 mapping. The payload
`ecall`s back to M-mode. M-mode (`mid_to_B`) snapshots the trap
record, rewrites the leaf PTE to point at PA2, and issues **no
fence**. Phase B runs in S-mode: the load must return **stale**
canary A, proving the PTE rewrite was invisible to the hart. The
payload `ecall`s back. M-mode (`mid_to_C`) snapshots the trap
record and executes `sfence.vma`. Phase C runs in S-mode: the
load must return **fresh** canary B. The report snapshots the
final trap record, restores `satp` to Bare and the PMP entry to 0,
prints every measured value, runs the checks, takes a 2M-iteration
quiet window, and prints the verdict.

## Results (QEMU 8.2.2, 3 runs byte-identical, md5 b035b8b0546e4c38718a619b4111f872)

- `satp` readback `0x8000000000080009` (MODE=8 Sv39, ASID=0, root
  PPN correct); `pte_before = 0x200010c7` (PA1 leaf),
  `pte_after = 0x20000cc7` (PA2 leaf, the software change landed
  in memory).
- Phase A readback `0xaaaaaaaaaaaaaaaa` (fresh walk to PA1).
- Phase B readback `0xaaaaaaaaaaaaaaaa`: the stale mapping, with
  the PTE already rewritten in memory and no fence issued.
- Phase C readback `0xbbbbbbbbbbbbbbbb`: the fresh mapping after
  `sfence.vma`.
- `m_traps = 3`, each with `mcause = 0x9` and `mepc` exactly at
  its phase's `ecall` site; `s_traps = 0`; quiet window moved
  neither counter.
- 39 checks, 0 mismatches, FNV-1a `0x421dacf7967027dc`
  byte-identical across 3 QEMU 8.2.2 runs, QEMU exit 0 on all
  three, Verdict PASS.

## Why this is the real mechanism

The stale phase-B readback is not a quirk of the test setup: the
PTE readback in the same run proves the rewrite landed in memory
(`pte_after` points at PA2), and the phase-C readback proves the
same load instruction then returns the new page's contents once
the fence orders the cache. The contrast is the mechanism: the
write to the page table is ordered by software, but the hart's
use of it is ordered by `sfence.vma`. No other instruction is
issued between the rewrite and the phase-B load that could
invalidate the cached translation.

## Build and run

```
make sfence-vma-remap.elf
make run-sfence-vma-remap QEMU=~/workspace/qemu/usr/bin/qemu-system-riscv64
```

Toolchain: Ubuntu `gcc-riscv64-unknown-elf` 13.2.0
(`~/workspace/toolchains/ubuntu-rv64`). PASS writes `0x5555` to
the virt test-device finisher, so QEMU exits 0; on FAIL the hart
parks.
