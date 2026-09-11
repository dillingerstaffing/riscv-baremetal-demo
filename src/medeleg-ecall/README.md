# medeleg bit-9 S-mode ecall delegation routing

Proves that `medeleg` bit 9 routes an S-mode `ecall` to the
S-mode handler when set and to the M-mode handler when clear,
with the exact trap cause codes and handler-mode counts on both
sides.

## What it does

1. Records the boot `medeleg` value and probes bit-9 writability
   (write `0x200`, require the readback to have bit 9 set), so the
   later delegation state is proven to take effect.
2. Installs direct-mode `mtvec` and `stvec` handlers with
   `mscratch`/`sscratch` save areas, and opens a
   whole-address-space PMP NAPOT entry (S-mode is default-deny
   without one).
3. Phase 1 with `medeleg` bit 9 set: drops to S-mode with `mret`
   and executes `ecall`. Requires exactly one S-mode trap with
   `scause = 9`, `sepc` at the ecall site, `sepc+4` at the resume
   label, `sstatus.SPP = 1`, and zero M-mode traps.
4. Returns to M-mode via a deliberate illegal instruction (cause 2
   stays in `medeleg`); the handler jumps to phase 2. Requires the
   M-mode trap count to be exactly 1, clears `medeleg`, and
   requires the readback to be 0.
5. Phase 2 with `medeleg` clear: drops to S-mode and executes
   `ecall` again. Requires the M-mode handler to record
   `mcause = 9` with `mepc` at the ecall site, the M-mode trap
   count to advance to 2, and the S-mode trap count to stay
   unchanged.
6. Verdict: M-mode trap count 3, phase-2 ecall count 1,
   phase-complete count 2, S-mode trap count unchanged. Prints an
   FNV-1a checksum over the verdict values; all three runs are
   byte-identical.

## Files

- `med_main.c`: M-mode setup, both S-mode payloads, phase-2 and
  verdict functions, UART reporting, PASS/FAIL verdict, and the
  virt test-device finisher shutdown.
- `med_trap.S`: M-mode trap entry; records `mcause`/`mepc`,
  services the phase-2 ecall, and routes the phase-complete
  illegal instructions to `med_phase2`/`med_finish`.
- `med_strap.S`: S-mode trap entry; records `scause`/`stval`/
  `sepc`/`sstatus.SPP` and skips the ecall.
- `PROOF.md`: the full proof record with per-run measurements.
- `bench-logs/`: the build log and the three raw QEMU run logs.

## Build and run

Built manually (no Makefile stanza, same style as the
scounteren-trap module), from the repo root with
`riscv64-unknown-elf-gcc` and flags `-Wall -Wextra -O2
-ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic
-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`; see PROOF.md
for the exact commands. Run:

```
qemu-system-riscv64 -machine virt -nographic -bios none -kernel medeleg-ecall.elf
```

PASS is reported as `RESULT: PASS` with `checks=17
mismatches=0` and the finisher shuts the machine down, so QEMU
exits 0. FAIL parks the hart in a `wfi` loop without touching the
finisher; under `timeout` that shows up as exit status 124.
