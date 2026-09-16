# medeleg-store-pagefault

Checks where a store-page-fault trap raised in S-mode lands as
`medeleg` bit 15 flips, under QEMU 8.2.2 in M-mode with a
hand-built Sv39 page table. Sibling of the `medeleg-instr-pagefault`
module (bit 12) and the `medeleg-load-pagefault` module (bit 13):
same tables, same two-phase structure, but the S-mode payload
issues a store to the unmapped page instead of jumping to it or
loading from it.

M-mode builds the tables by hand: `root[2]` -> `l1_id[0]` ->
`l0_id[]`, identity-mapping `[0x80000000, 0x80080000)` with R|W|X
and leaving `l0_id[128]` zero (invalid), so a store to the fault
page at `0x80080000` dies at the leaf lookup. One PMP NAPOT entry
grants S-mode R|W|X over the whole address space; Sv39 is enabled
via `satp` + `sfence.vma`.

Phase A writes `medeleg` bit 15 (readback `0x8000`, bit 15 verified
set), drops to S-mode, and stores to the fault page: the trap must
land in S-mode with `scause = 0xf`, `sepc` exactly at the faulting
store instruction and `stval` exactly at the faulting data address
(the two differ, which is what distinguishes a store page fault from
an instruction page fault), while the M-mode handler sees only the
phase-A return `ecall`. The M-mode handler records the `ecall` and
redirects into the phase-B setup. Phase B clears bit 15 (readback
`0x0`), drops to S-mode, and stores to the same page: the trap
must land in M-mode with `mcause = 0xf`, `mepc` exactly at the
faulting store and `mtval` exactly at the fault page, with the
S-mode trap count unchanged. The run publishes the per-mode trap
counts, causes, PCs, trap values, and both `medeleg` write/readback
pairs, runs 34 checks, and writes the FNV-1a checksum over the
verdict values so repeated runs can be compared byte for byte.

Files:

- `mspf_main.c`: two-phase driver, hand-built Sv39 tables with
  per-PTE verification, UART reporting, checks, checksum,
  PASS/FAIL verdict with virt test-device finisher shutdown on PASS
  and a parked hart on FAIL.
- `mspf_trap.S`: M-mode and S-mode trap entries (record, count, and
  resume/redirect).
- `PROOF.md`: proof log with the machine-readable header, the
  measured values, and the verification record.
- `bench-logs/`: build log and the three raw QEMU run logs.

Build and run from the repo root (CROSS to taste):

    make medeleg-store-pagefault.elf
    make run-medeleg-store-pagefault

On a passing run QEMU exits 0 after `RESULT: PASS (checks=34)`.
