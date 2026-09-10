# pmp-tor: PMP TOR boundary test (backlog item 103)

Two PMP entries in TOR (top-of-range) mode form one exact boundary:
entry 0 allows `[0, 0x80002000)`, entry 1 denies `[0x80002000,
0x80003000)` (4 KiB scratch) with the lock bit set. An `lbu` of the
last allowed byte (`0x80001FFF`) completes with no trap and returns the
`0x5A` sentinel; an `lbu` of the first denied byte (`0x80002000`) traps
with `mcause=0x5` (load access fault), `mepc=0x80000272` exactly the
faulting instruction (`resume - 8`, verified against the disassembly),
`mtval=0x80002000` exactly the faulting address. Lock verified by
attempted clear (`pmpcfg0` `0x880f` -> `0x8800`: entry 1 byte holds,
entry 0 byte clears). Three QEMU 8.2.2 runs, byte-identical,
`RESULT: PASS` each.

Build: `make pmp-tor.elf` (repo root). Run: `make run-pmp-tor`
(or `qemu-system-riscv64 -machine virt -nographic -bios none -kernel pmp-tor.elf`).

Files: `pmt_main.c` (PMP programming, controls, probes, verdict),
`pmt_trap.S` (M-mode trap entry recording mcause/mepc/mtval),
`PROOF.md` (full writeup), `bench-logs/` (build log + 3 raw run logs).

See `PROOF.md` for the configuration, the defect found during
development (a wrong config byte, `0x89` vs `0x88`, caught via QEMU's
own PMP source), and the limits of verification.
