# pmp-priority

Checks PMP entry priority: two unlocked NAPOT entries are programmed
over the same 4 KiB scratch page with the same `pmpaddr` value and
conflicting permissions, and an S-mode load from the page is observed
as the entries are swapped (the lowest-numbered matching entry wins).

A third, higher-numbered NAPOT entry allows R|W|X over
`[0x80000000, 0x100000000)` so S-mode code fetch and a control load
keep working; `medeleg` stays 0 and `satp` stays Bare, so no page
tables are needed and the probe address is the physical page address.
Phase A (entry 0 allow, entry 1 deny): the S-mode `lbu` of a canary
completes with no trap, and an `ecall` returns to M-mode. Phase B
(`pmpcfg0` rewritten with entry 0 deny, entry 1 allow): the same `lbu`
traps in M-mode with `mcause = 0x5` (load access fault), `mepc` at the
faulting load and `mtval` at the scratch page, recorded by the M-mode
handler. Each phase first loads a control byte covered only by the
broad allow entry (completes in both phases), a quiet spin window moves
neither trap counter, and the boot PMP config is restored and reported
at the end. The run publishes the per-phase `pmpcfg0`/`pmpaddr`
readbacks, the trap counts, causes, PCs, and trap values, and an
FNV-1a checksum over the verdict values so repeated runs can be
compared byte for byte.

Two NAPOT entries (not TOR, which cannot overlap) express the
conflicting match: TOR ranges are half-open and partition the address
space, so two TOR entries can never both match one address. This
distinguishes the module from the `pmp-tor` module (match semantics)
and the `pmp-lock-bit` module (locking): the mechanism here is entry
priority only.

Files:

- `pp_main.c`: two-phase driver, three-entry PMP setup with
  per-register readback verification, M -> S drop via `sret`, UART
  reporting, checks, checksum, PASS/FAIL verdict with virt
  test-device finisher shutdown on PASS and a parked hart on FAIL.
- `pp_trap.S`: M-mode and S-mode trap entries (record, count,
  and resume/redirect).
- `PROOF.md`: proof log with the machine-readable header, the
  measured values, and the verification record.
- `bench-logs/`: build log and the three raw QEMU run logs.

Build and run from the repo root (CROSS to taste):

    make pmp-priority.elf
    make run-pmp-priority

On a passing run QEMU exits 0 after `RESULT: PASS`.
