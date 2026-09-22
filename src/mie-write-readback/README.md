# mie-write-readback

`mwr_trap.S`, `mwr_main.c`: M-mode WARL write/readback probe of the
mie interrupt-enable CSR. Writes all-ones to mie and publishes the
legalized readback (which enable bits the implementation provides),
writes zero and confirms the readback is 0, then probes bits 0..11
individually (each must read back exactly the written bit or 0, with
no stray bits), writes all-ones a second time and confirms the
readback matches the first and its low 12 bits match the OR of the
per-bit results, then restores mie to the exact boot value.
mstatus.MIE stays clear for the whole run so no interrupt can be
taken, and a trap handler is installed but must never fire (trap
count 0).

Build: `make mie-write-readback.elf` in the repo root.
Run: `make run-mie-write-readback` (QEMU virt, `-bios none`),
under `timeout`.

The measured results and limits are in `PROOF.md`; the raw build
and run logs are in `bench-logs/`.
