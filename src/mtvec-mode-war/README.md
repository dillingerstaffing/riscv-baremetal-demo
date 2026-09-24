# mtvec-mode-war

`mmw_trap.S`, `mmw_main.c`: M-mode WARL write/readback probe of the
mtvec CSR's MODE field. Records the boot mtvec readback, writes
all-ones and publishes the readback (a write with a reserved MODE
encoding is dropped entirely on this implementation, the previous
value is preserved), writes an all-ones BASE with MODE=0 and
confirms every BASE bit sticks, writes MODE=1 (vectored) and
MODE=0 (direct) values at the handler address and publishes each
readback (both stick, so both modes are provided), then restores
mtvec to the exact boot value and verifies mstatus is unchanged
bit-for-bit. mstatus.MIE stays clear for the whole run so no
interrupt can be taken, and a trap handler is installed but must
never fire (trap count 0).

Build: `make mtvec-mode-war.elf` in the repo root.
Run: `make run-mtvec-mode-war` (QEMU virt, `-bios none`),
under `timeout`.

The measured results and limits are in `PROOF.md`; the raw build
and run logs are in `bench-logs/`.
