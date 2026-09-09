# ecall ABI round-trip

An S-mode payload issues environment calls and an M-mode trap handler
answers them, verifying the calling convention across the privilege
boundary.

## What was built

`src/ecall/` (443 lines, shares only `src/boot.S` and `src/uart.c`
with the other demos; `make ecall.elf`, run with `make run-ecall`):

- The program drops from M-mode to S-mode (sepc + sstatus.SPP + sret,
  with a PMP NAPOT R/W/X entry covering the address space, since
  lower privilege modes default-deny with no PMP entry programmed).
- The S-mode payload loads three sets of known constants into a0-a7
  and t0-t6, issues an ecall with service number 1 in a7, and reads
  every register back. Register pinning uses `register ...
  __asm__("a0")` variables so the compiled `ecall` has the exact
  layout (confirmed in the disassembly).
- The M-mode trap handler (`ecall_trap.S` + a C dispatcher) saves all
  of x1-x31, logs mcause/mepc/the trapped instruction word plus the
  received registers, returns a0 = a0^a1^a2^a3^a4^a5, restores
  everything else, and resumes past the ecall.
- The M-mode report prints both sides, handler-received vs
  payload-loaded, and cross-checks them word for word.

## What was measured

Three runs under QEMU 8.2.2, byte-identical output, all `RESULT: PASS`:

- Every call trapped with mcause = 9 (S-mode environment call) and
  the trapped instruction word read 0x00000073 (ecall).
- Handler-received a0-a5 matched the loaded constants exactly, all
  three sets; a6, a7, t0-t6 likewise.
- Returned a0 values: 0x7, 0x8000000000000001, 0xd1a2b1e0c5f1b5b1,
  each equal to the XOR of its argument set (recomputed independently).
- After each ecall the payload read back a1-a7 and t0-t6 bit-identical
  to what it loaded, including edge values 0, 0xFFFFFFFFFFFFFFFF,
  0xAAAAAAAAAAAAAAAA, and 0x5555555555555555.

See `PROOF.md` for the full build log, raw run output, and the limits
of verification (emulator behavior, single hart, no virtual memory,
synchronous ecall path only).
