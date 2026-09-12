# mip.MSIP software-write probe (backlog item "riscv mip-msip-write")

Verified finding on QEMU 8.2.2: `mip` bit 3 (MSIP) is not
software-writable through the CSR. Writes via `csrsi`/`csrci` are
ignored (WARL): the readback is unchanged and the bit never sets.
The bit is driven by the CLINT `msip` MMIO register instead, which
is the mechanism `src/mip-pending-no-trap/` exercises. The two
modules test different mechanisms and share no behavior.

The module proves the negative with a control: the same
immediate-form CSR write sets `mip` bit 1 (SSIP) and reads back,
so the write path itself works and the ignored write is specific
to MSIP.

## What it does

1. Boots in M-mode, records the `mip`/`mie`/`mstatus` boot readbacks
   (0x80 / 0x0 / 0xa00000000; MTIP pends at boot because `mtimecmp`
   reads 0 at reset) and installs a counting park-on-entry trap
   vector (direct-mode `mtvec`). Both interrupt enables stay clear
   for the whole run, so no trap should ever fire.
2. Control: sets `mip.SSIP` via `csrsi mip, 2`; the readback is
   0x82, the bit set and the write path proven. Clears it via
   `csrci mip, 2`; the readback returns to 0x80.
3. Attempts `csrsi mip, 8` (bit 3, MSIP); the readback stays 0x80,
   the bit unset: the write is ignored.
4. Spins a bounded window (1,000,000 iterations) with the enables
   still clear; `mip` still reads 0x80 and the trap count stays 0
   (pending MTIP undelivered).
5. Attempts `csrci mip, 8`; the readback stays 0x80: the clear is
   likewise ignored.
6. Verifies the enables are still clear, prints the trap count (0),
   a 64-bit FNV-1a checksum over the deterministic measured values,
   and `RESULT: PASS` only if all 17 checks held. On PASS it shuts
   the machine down via the virt test-device finisher so the QEMU
   process exits 0; on FAIL it parks the hart instead.
