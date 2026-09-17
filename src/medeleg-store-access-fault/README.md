# medeleg-store-access-fault

Checks where a store-access-fault trap raised in S-mode lands as
`medeleg` bit 7 flips, under QEMU 8.2.2 in M-mode with a
hand-built Sv39 page table and a hand-built PMP TOR layout.
Sibling of the `medeleg-store-pagefault` module (bit 15, store
page fault on an invalid PTE): same tables, same two-phase
structure, but here the leaf PTE over the store page is VALID
with R|W, so translation succeeds and the fault comes from a
locked PMP TOR deny entry covering exactly the store page. A PMP
deny raises an access fault, never a page fault, because PMP is
checked on the physical address after the page walk succeeds.

M-mode builds the tables by hand: `root[2]` -> `l1_id[0]` ->
`l0_id[]`, identity-mapping `[0x80000000, 0x80080000)` with R|W|X
and `l0_id[128]` valid with R|W (no X) over the store page at
`0x80080000`. Three PMP TOR entries, lowest-numbered match wins:
entry 0 allows `[0, 0x80080000)` R|W|X unlocked, entry 1 LOCKED
denies `[0x80080000, 0x80081000)` (exactly the store page), entry
2 allows `[0x80081000, 0x100000000)` R|W|X unlocked; every
`pmpcfg`/`pmpaddr` register is read back and verified. Sv39 is
enabled via `satp` + `sfence.vma`.

Phase A writes `medeleg` bit 7 (readback `0x80`, bit 7 verified
set), drops to S-mode, and stores to the denied page: the trap
must land in S-mode with `scause = 0x7`, `sepc` exactly at the
faulting store instruction and `stval` exactly at the faulting
data address (the two differ, which is what distinguishes a store
access fault from an instruction access fault), while the M-mode
handler sees only the phase-A return `ecall`. The M-mode handler
records the `ecall` and redirects into the phase-B setup. Phase B
clears bit 7 (readback `0x0`), drops to S-mode, and stores to the
same page: the trap must land in M-mode with `mcause = 0x7`,
`mepc` exactly at the faulting store and `mtval` exactly at the
store page, with the S-mode trap count unchanged. Each phase
first stores to an allowed scratch word (control, completes with
no trap, proving the fault comes from the PMP entry), a quiet
window moves neither trap counter, and the boot `medeleg` is
restored and reported at the end. The run publishes the per-mode
trap counts, causes, PCs, trap values, both `medeleg`
write/readback pairs, the PMP readbacks, and an FNV-1a checksum
over the verdict values, runs 41 checks, and writes the checksum
so repeated runs can be compared byte for byte.

Files:

- `msaf_main.c`: two-phase driver, three-entry PMP TOR setup
  with per-register readback verification, hand-built Sv39
  tables with per-PTE verification, UART reporting, checks,
  checksum, PASS/FAIL verdict with virt test-device finisher
  shutdown on PASS and a parked hart on FAIL.
- `msaf_trap.S`: M-mode and S-mode trap entries (record, count,
  and resume/redirect).
- `PROOF.md`: proof log with the machine-readable header, the
  measured values, and the verification record.
- `bench-logs/`: build log and the three raw QEMU run logs.

Build and run from the repo root (CROSS to taste):

    make medeleg-store-access-fault.elf
    make run-medeleg-store-access-fault

On a passing run QEMU exits 0 after `RESULT: PASS (checks=41)`.
