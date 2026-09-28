# sfence-vma-rs1-scoped

Checks whether `sfence.vma` with `rs1` set scopes the TLB
invalidation to the named address, under QEMU 8.2.2 in M-mode
with a hand-built Sv39 page table.

M-mode builds the tables by hand: `root[2]` -> `l1_id[0]` ->
`l0_id[]` identity-maps `[0x80000000, 0x80080000)` with R|W|X,
`root[1]` -> `l1_t1[0]` -> `l0_t1[0]` maps `VA1 = 0x40000000` to
a page holding canary A (`0xaaaaaaaaaaaaaaaa`), and `root[3]` ->
`l1_t2[0]` -> `l0_t2[0]` maps `VA2 = 0xc0000000` to a page
holding canary C (`0xcccccccccccccccc`). Two more pages hold
canaries B and D. One PMP NAPOT entry grants S-mode R|W|X over
the whole address space; Sv39 is enabled via `satp` +
`sfence.vma`.

Phase A reads both words in S-mode: expect canaries A and C,
caching both translations. M-mode rewrites both leaf PTEs (VA1
to the B page, VA2 to the D page) and issues **only**
`sfence.vma` with `rs1 = VA1` (disassembly confirms nonzero
`rs1`, zero `rs2`). Phase B reads both words: VA1 must read
fresh canary B, and the module records whether VA2 reads stale
C or fresh D. M-mode issues a global `sfence.vma`. Phase C
reads both words: expect B and fresh D.

Measured on QEMU 8.2.2, the scoped fence does **not** scope:
phase B reads fresh B at VA1 *and* fresh D at VA2, so the hart
dropped VA2's cached translation too. The sibling module
`src/sfence-vma-remap` is the control (no fence at all reads
stale), so VA2's fresh readback is the fence over-invalidating,
not a missing cache entry. Over-invalidation is still correct,
just not scoped, and the global fence in the last phase is a
no-op confirmation. The run publishes all six readbacks, both
PTEs before and after, per-mode trap counts, causes,
ecall-site PCs, runs 60 checks, and writes the FNV-1a checksum
over the verdict values so repeated runs can be compared byte
for byte.

Files:

- `srs_main.c`: three-phase driver, hand-built Sv39 tables with
  per-PTE verification, UART reporting, checks, checksum,
  PASS/FAIL verdict with virt test-device finisher shutdown on PASS
  and a parked hart on FAIL.
- `srs_trap.S`: M-mode and S-mode trap entries (record, count,
  and redirect; the S-mode entry parks, since no S-mode trap is
  ever expected).
- `PROOF.md`: proof log with the machine-readable header, the
  measured values, and the verification record.
- `bench-logs/`: the three raw QEMU run logs.

Build and run from the repo root (CROSS to taste):

    make sfence-vma-rs1-scoped.elf
    make run-sfence-vma-rs1-scoped

On a passing run QEMU exits 0 after `RESULT: PASS (checks=60)`.
