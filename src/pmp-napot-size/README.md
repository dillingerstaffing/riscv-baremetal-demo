# pmp-napot-size: PMP NAPOT size-decoding test (backlog item 118)

Two locked no-access PMP entries in NAPOT mode over one 64 KiB
scratch region (base `0x80020000`, 64 KiB aligned), at two different
encoded sizes. Entry 0: `pmpaddr0=0x200081ff` (trailing-ones `0x1ff`,
9 ones, 4 KiB, `[0x80020000, 0x80021000)`). Entry 1:
`pmpaddr1=0x20009fff` (trailing-ones `0x1fff`, 13 ones, 64 KiB,
`[0x80020000, 0x80030000)`). Both config bytes `0x98` (L=1, A=NAPOT,
R=W=X=0); `pmpcfg0` reads back `0x9898` after entry 1 is added, proving
the locked byte 0 held while byte 1 was installed.

Phase A (4 KiB encoding only): `lbu` at `0x80022000` (inside a 64 KiB
window, outside the 4 KiB window) completes with no trap and returns the
pattern byte `0x00`; `lbu` at `0x80020fff` (last byte inside) traps with
`mcause=0x5` (load access fault), `mepc=0x80000270` exactly the faulting
instruction (`resume - 8`, verified against the disassembly),
`mtval=0x80020fff` exactly the faulting address; `lbu` at `0x80021000`
(first byte outside) completes with no trap, byte `0x00`.

Phase B (64 KiB encoding added): the same `lbu` at `0x80022000` now
traps with `mcause=0x5`, `mepc=0x80000270`, `mtval=0x80022000`; `lbu` at
`0x8002ffff` (last byte inside the large region) traps with `mcause=0x5`,
`mtval=0x8002ffff`; `lbu` at `0x80030000` (first byte outside) completes
with no trap; a dedicated sentinel byte outside the region reads back
`0xa5` with no trap. The fault/no-fault boundary moved exactly as the
encoded size predicts. Three QEMU 8.2.2 runs, byte-identical,
`RESULT: PASS` each.

Build: `make pmp-napot-size.elf` (repo root). Run: `make run-pmp-napot-size`
(or `qemu-system-riscv64 -machine virt -nographic -bios none -kernel pmp-napot-size.elf`).

Files: `pns_main.c` (PMP programming, controls, probes, verdict),
`pns_trap.S` (M-mode trap entry recording mcause/mepc/mtval),
`PROOF.md` (full writeup), `bench-logs/` (build log + 3 raw run logs).

See `PROOF.md` for the configuration, the defect found during
development (a sentinel write that aliased the link-placed `fails`
counter at `base+0x10000`), and the limits of verification.
