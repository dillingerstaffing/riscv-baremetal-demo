# medeleg-instr-pagefault

Checks where an instruction-page-fault trap raised in S-mode lands
as `medeleg` bit 12 flips, under QEMU 8.2.2 in M-mode with a
hand-built Sv39 page table.

M-mode builds the tables by hand: `root[2]` -> `l1_id[0]` ->
`l0_id[]`, identity-mapping `[0x80000000, 0x80080000)` with R|W|X
and leaving `l0_id[128]` zero (invalid), so an instruction fetch
from the fault page at `0x80080000` dies at the leaf lookup. One
PMP NAPOT entry grants S-mode R|W|X over the whole address space;
Sv39 is enabled via `satp` + `sfence.vma`.

Phase A writes `medeleg` bit 12 (readback `0x1000`, bit 12 verified
set), drops to S-mode, and jumps to the fault page: the trap must
land in S-mode with `scause = 0xc` and both `sepc` and `stval`
exactly at the faulting fetch address, while the M-mode handler
sees only the phase-A return `ecall`. The M-mode handler records
the `ecall` and redirects into the phase-B setup. Phase B clears
bit 12 (readback `0x0`), drops to S-mode, and jumps to the same
page: the trap must land in M-mode with `mcause = 0xc` and both
`mepc` and `mtval` exactly at the faulting fetch address, with the
S-mode trap count unchanged. The run publishes the per-mode trap
counts, causes, PCs, trap values, and both `medeleg` write/readback
pairs, runs 32 checks, and writes the FNV-1a checksum over the
verdict values so repeated runs can be compared byte for byte.

Files:

- `mipf_main.c`: two-phase driver, hand-built Sv39 tables with
  per-PTE verification, UART reporting, checks, checksum,
  PASS/FAIL verdict with virt test-device finisher shutdown on PASS
  and a parked hart on FAIL.
- `mipf_trap.S`: M-mode and S-mode trap entries (record, count, and
  resume/redirect).
- `PROOF.md`: proof log with the machine-readable header, the
  measured values, and the verification record.
- `bench-logs/`: build log and the three raw QEMU run logs.

Build and run from the repo root (CROSS to taste):

    make medeleg-instr-pagefault.elf
    make run-medeleg-instr-pagefault

On a passing run QEMU exits 0 after `RESULT: PASS (checks=32)`.
