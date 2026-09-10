# mideleg selective routing (backlog item 153)

Sets `mideleg` to delegate only the supervisor external interrupt
(bit 9, readback-verified), then fires a supervisor timer interrupt
and a PLIC supervisor external interrupt, and checks where each one
lands: the timer interrupt must trap to M-mode, the external
interrupt to S-mode.

## What it does

1. Boots in M-mode, installs a minimal M-mode trap handler
   (direct-mode `mtvec`, `mscratch` scratch area) that records
   `mcause`/`mepc`, clears a pended `STIP` bit, and services an
   S-mode `ecall` readback request for the M-mode-only `mideleg`
   CSR.
2. Records the boot `medeleg`/`mideleg` values, writes zero to
   `mideleg` and reads back the forced set, then writes bit 9 and
   requires the readback to equal the forced set with only bit 9
   added (the write changed nothing else). Bit 5 must read back
   clear. `medeleg` is written zero and read back zero.
3. Phase (a): pends the supervisor timer interrupt by writing
   `STIP` in `mip` with `mie.STIE` and `mstatus.MIE` set. It is not
   delegated, so it must trap to M-mode with
   `mcause = 0x8000000000000005`, exactly once.
4. Installs `stvec`, opens the whole address space to S-mode with
   one PMP NAPOT entry, programs the PLIC hart-0 S-mode context
   (priority 1 for UART source 10, enable bit 10, threshold 0, every
   write read back), and asserts the UART interrupt with one
   looped-back byte (same construction as `src/plic/`).
5. Drops to S-mode via `sret`, enables `sie.SEIE` and
   `sstatus.SIE`. The pending interrupt is taken at the next
   instruction boundary; the handler claims it through the S-mode
   context claim register, reads the looped-back byte (dropping the
   IRQ line), completes the claim, and raises a done flag. The
   program requires `scause = 0x8000000000000009`, `sepc` equal to
   the address of the interrupted instruction (captured with an
   in-assembly label), and claim id 10.
6. Reads back `mideleg` via the M-mode `ecall` service and requires
   it unchanged; prints `RESULT: PASS` only if all 15 checks held.

Measured finding: on this hart (QEMU 8.2.2, virt machine) the
hypervisor extension is present, so `mideleg` bits 2, 6, 10, 12
read 1 after every write (QEMU ORs the hypervisor interrupt bits
in after each write; see `target/riscv/csr.c`, `rmw_mideleg64`).
Zero-write readback is `0x1444`; writing `0x200` reads back
`0x1644`. Among the software-writable bits, only bit 9 was set.

## Files

- `midr_main.c`: the test sequence, UART reporting, PASS/FAIL
  verdict.
- `midr_trap.S`: the M-mode and S-mode trap entries.
- `PROOF.md`: the full proof record with per-run measurements.
- `bench-logs/`: the build log and the three raw QEMU run logs.

## Build and run

From the repo root:

```
make mideleg-route.elf
make run-mideleg-route
```

`RESULT: PASS` is printed only when every check holds; the hart
then parks in a `wfi` loop (run under `timeout`; exit status 124
is the harness killing the parked hart, not a failure).
