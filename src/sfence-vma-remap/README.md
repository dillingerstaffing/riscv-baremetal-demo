# sfence-vma-remap

Checks what a PTE remap does to the hart's cached translations
with and without `sfence.vma`, under QEMU 8.2.2 in M-mode with a
hand-built Sv39 page table.

M-mode builds the tables by hand: `root[2]` -> `l1_id[0]` ->
`l0_id[]` identity-maps `[0x80000000, 0x80080000)` with R|W|X, and
`root[1]` -> `l1_test[0]` -> `l0_test[0]` is the single leaf under
test, mapping `0x40000000` to a page holding canary
`0xaaaaaaaaaaaaaaaa`. A second page holds canary
`0xbbbbbbbbbbbbbbbb`. One PMP NAPOT entry grants S-mode R|W|X over
the whole address space; Sv39 is enabled via `satp` +
`sfence.vma`.

Phase A reads the word at `0x40000000` in S-mode: expect canary A,
and the read caches the translation. M-mode rewrites the leaf to
the second page with no fence. Phase B reads again: expect stale
canary A, proving the PTE rewrite was invisible to the hart.
M-mode executes `sfence.vma`. Phase C reads again: expect fresh
canary B. The run publishes all three readbacks, the PTE before
and after, per-mode trap counts, causes, ecall-site PCs, runs 39
checks, and writes the FNV-1a checksum over the verdict values so
repeated runs can be compared byte for byte.

Files:

- `svr_main.c`: three-phase driver, hand-built Sv39 tables with
  per-PTE verification, UART reporting, checks, checksum,
  PASS/FAIL verdict with virt test-device finisher shutdown on PASS
  and a parked hart on FAIL.
- `svr_trap.S`: M-mode and S-mode trap entries (record, count,
  and redirect; the S-mode entry parks, since no S-mode trap is
  ever expected).
- `PROOF.md`: proof log with the machine-readable header, the
  measured values, and the verification record.
- `bench-logs/`: the three raw QEMU run logs.

Build and run from the repo root (CROSS to taste):

    make sfence-vma-remap.elf
    make run-sfence-vma-remap

On a passing run QEMU exits 0 after `RESULT: PASS (checks=39)`.
