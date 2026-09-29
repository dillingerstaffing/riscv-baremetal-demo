# sfence-vma-rs2-scoped

Checks whether `sfence.vma` with `rs1 = x0` and `rs2` holding an
ASID scopes the TLB invalidation to that ASID, under QEMU 8.2.2
in M-mode with two hand-built Sv39 page tables.

M-mode builds the tables by hand: `root1` (ASID 1) has
`root1[1]` -> `l1a[0]` -> `l0a[0]` mapping `VA = 0x40000000` to
a page holding canary A (`0xaaaaaaaaaaaaaaaa`), and `root2`
(ASID 2) has `root2[1]` -> `l1b[0]` -> `l0b[0]` mapping the same
VA to a page holding canary B (`0xbbbbbbbbbbbbbbbb`). Two more
pages hold canaries C and D. Both roots share an identity map
of `[0x80000000, 0x80080000)` with R|W|X; one PMP NAPOT entry
grants S-mode R|W|X over the whole address space; Sv39 is
enabled via `satp` + `sfence.vma`.

Phase A reads VA under ASID 1: expect canary A, caching the
(ASID 1, VA) entry. Phase B installs ASID 2 and reads VA: expect
canary B, caching the (ASID 2, VA) entry. M-mode rewrites both
leaf PTEs (ASID 1 leaf to the C page, ASID 2 leaf to the D page)
with **no** fence and no `satp` write, then phase C reads VA
under ASID 2 with `satp` untouched: expect stale canary B, the
cached-stale baseline. M-mode issues **only** `sfence.vma` with
`rs1 = x0` and `rs2 = 1` (disassembly confirms `li a4,1` before
`sfence.vma zero,a4`), then phase D reads VA under ASID 2 and
records the verdict: stale B means correct ASID scoping, fresh D
means the fence over-invalidated. Phase E reinstalls ASID 1
with a full fence and reads VA: expect fresh canary C.

Measured on QEMU 8.2.2, the ASID-scoped fence does **not**
scope: phase D reads fresh D, so the hart dropped ASID 2's
cached translation too. Phase C is the control (no fence reads
stale), so phase D's fresh readback is the fence
over-invalidating, not a missing cache entry. This matches the
sibling module `src/sfence-vma-rs1-scoped`, which measured the
same full-flush behavior for the `rs1` operand. The run
publishes all five readbacks, both PTEs before and after,
per-mode trap counts, causes, ecall-site PCs, runs 82 checks,
and writes the FNV-1a checksum over the verdict values so
repeated runs can be compared byte for byte.

Files:

- `srs2_main.c`: five-phase driver, two hand-built Sv39 tables
  with per-PTE verification, UART reporting, checks, checksum,
  PASS/FAIL verdict with virt test-device finisher shutdown on
  PASS and a parked hart on FAIL.
- `srs2_trap.S`: M-mode and S-mode trap entries (record, count,
  and redirect; the S-mode entry parks, since no S-mode trap is
  ever expected).
- `PROOF.md`: proof log with the machine-readable header, the
  measured values, and the verification record.
- `bench-logs/`: the three raw QEMU run logs.

Build and run from the repo root (CROSS to taste):

    make sfence-vma-rs2-scoped.elf
    make run-sfence-vma-rs2-scoped

On a passing run QEMU exits 0 after `RESULT: PASS (checks=82)`.
