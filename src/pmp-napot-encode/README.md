# pmp-napot-encode: PMP NAPOT encoding write/readback test (backlog item "riscv pmp-napot-encode")

M-mode only, no traps expected: the address-matching encoding is
checked directly on `pmpaddr0` and the A field on `pmpcfg0`, with
every entry keeping L = 0, so unlocked PMP entries cannot restrict
M-mode at all. NAPOT encodes a naturally aligned power-of-two
region as `pmpaddr = (base >> 2) | ((size >> 3) - 1)`: the address
above the region's granularity bits plus a tail of trailing ones
whose count names the size.

The run records boot `pmpaddr0`/`pmpaddr1`/`pmpcfg0` (all `0x0` on
QEMU 8.2.2), then writes three patterns to `pmpaddr0`, each
required to read back exactly as written:

- `0x200009ff` (4 KiB at `0x80002000`, 9 trailing ones)
- `0x20101fff` (64 KiB at `0x80400000`, 13 trailing ones)
- `0x2201ffff` (1 MiB at `0x88000000`, 17 trailing ones)

Then `pmpcfg0 = 0x18` (entry 0: A = NAPOT, `0b11`): the A field must
read 3, and the entry-0 byte must read `0x18` exactly. Then
`pmpcfg0 = 0x1f` (entry 0: NAPOT + R + W + X, L clear): the byte
must read `0x1f` exactly, showing the permission bits coexist with
the address-matching field. Finally all three CSRs are restored to
their boot values, and each restore readback must match boot
exactly.

9 checks, 0 fails across 3 byte-identical QEMU 8.2.2 runs,
`RESULT: PASS` each; FNV-1a record checksum `0x7abdfe9a3880db7b`.

Build: `make pmp-napot-encode.elf` (repo root). Run:
`make run-pmp-napot-encode` (or `qemu-system-riscv64 -machine virt
-nographic -bios none -kernel pmp-napot-encode.elf`).

Files: `pne_main.c` (CSRs, checks, table, checksum, verdict),
`PROOF.md` (full writeup), `bench-logs/` (build log + 3 raw run
logs).

See `PROOF.md` for the exact written-vs-readback table and the
limits of verification (three sizes on entry 0 only; no PMP
permission enforcement is exercised here, that is the
`pmp-napot-size` module's job).
