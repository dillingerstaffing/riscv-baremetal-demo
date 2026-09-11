<!-- PROOF-HEADER
Checks: 10
Mismatches: 0
Checksum: 0xeb198f5eab8207a9
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: medeleg bit-8 U-mode ecall route (backlog item "riscv medeleg-ecall-u-route")

## What was built

`src/medeleg-ecall-u-route/`: a bare-metal RISC-V program that
issues one `ecall` from U-mode with `medeleg` bit 8 set and checks
the trap lands in S-mode with `scause = 8` (environment call from
U-mode) and `sepc` at the ecall, with zero M-mode traps during the
delegation phase. A restore `ecall` issued afterwards from S-mode
(medeleg bit 9 clear) is taken in M-mode, whose handler writes the
boot `medeleg` value back and records the readback. Four files,
sharing only `src/boot.S` and `src/uart.c` with the other demos.
Exactly one mechanism is under test: the routing of a U-mode
environment call by `medeleg` bit 8. This is the U-mode
counterpart of `src/medeleg-ecall-destination/`, which routed the
S-mode ecall on bit 9; the privilege transition tested here is
different (U-mode to S-mode).

- `umede_trap.S`: M-mode trap entry (records `mcause`/`mepc`/
  `mstatus` in `m_regs`; on the restore ecall writes the boot
  `medeleg` back, records the readback, and `mret`s to the
  finalizer with `mstatus.MPP` = M-mode; on a premature M-mode
  trap raises a flag in slot 7 and resumes at the finalizer so
  the evidence is reported instead of parking silently) and
  S-mode trap entry (records `scause`/`sepc`/`sstatus` in
  `s_regs`, raises the done flag, redirects to the S-mode
  reporter with `sstatus.SPP` = 1).
- `umede_main.c`: installs direct-mode `mtvec`/`mscratch` and
  `stvec`/`sscratch`, records the boot `medeleg`, writes `0x100`
  with an exact-readback check, opens the address space to lower
  modes with one PMP NAPOT entry, disarms `mie`/`mstatus.MIE`,
  drops to U-mode via `sret` with `sstatus.SPP` = 0; the U-mode
  payload captures the ecall address with an in-assembly label
  and issues the ecall; the S-mode reporter prints every measured
  value, runs the 7 delegation checks, takes a quiet window, arms
  the restore and issues the restore ecall from S-mode; the
  M-mode finalizer runs the 3 restore checks, prints an FNV-1a
  checksum over the verdict values, and prints `RESULT: PASS`
  only when all 10 checks held.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `make medeleg-ecall-u-route.elf` (added to `all` in the
Makefile; `src/boot.S` stays first in `UMUR_SRCS` so `_start`
lands at `0x80000000`, the address QEMU's `-kernel` loader
starts at). Zero compiler warnings.
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel medeleg-ecall-u-route.elf`
(or `make run-medeleg-ecall-u-route`).

Toolchain: Debian `riscv64-unknown-elf-gcc` 13.2.0
(`-march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`),
QEMU 8.2.2 (`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart. QEMU boots the ELF straight into
  M-mode with `-bios none`.
- The program is the only code running. The delegation phase runs
  in U-mode after the `sret` drop (one PMP NAPOT entry opens the
  whole address space to lower modes); the reporter runs in
  S-mode; the restore runs in M-mode.
- All interrupt enables stay clear for the whole run (`mie` = 0,
  `mstatus.MIE` clear, `sie`/`sstatus.SIE` never set), so no
  interrupt of either kind can fire; the only traps are the two
  ecalls.

## The 10 checks

Delegation (run by the S-mode reporter):
1. `medeleg` readback after writing `0x100` equals `0x100`
   exactly (bit 8 set, nothing else set).
2. S-mode trap count is exactly 1.
3. `scause` = 8 (environment call from U-mode, per the RISC-V
   privileged specification; cause 8 is derived from the
   privilege mode at trap time, so it is independent evidence
   the payload really ran in U-mode).
4. `sepc` equals the captured U-mode ecall address.
5. `sstatus.SPP` at trap entry = 0 (arrival from U-mode).
6. M-mode trap count is 0 during the delegation phase.
7. Quiet window (2M spin iterations, nothing pending): M-mode
   and S-mode trap counts do not move.

Restore (run by the M-mode finalizer):
8. Restore-trap `mcause` = 9 (environment call from S-mode;
   medeleg bit 9 is clear so it is taken in M-mode).
9. Restore-trap `mepc` equals the captured restore-ecall
   address.
10. `medeleg` readback after the restore write equals the boot
    value (`0x0` on QEMU 8.2.2 virt), so the machine is left as
    found.

## Measured values (run 1; runs 2 and 3 byte-identical)

```
medeleg-ecall-u-route: U-mode ecall route test
boot: medeleg=0x0 write=0x100 readback=0x100
u-ecall: s_traps=1 scause=0x8 sepc=0x800002f2 expected=0x800002f2 sstatus_spp=0 m_traps=0
quiet: m_traps=0 s_traps=1
restore: m_traps=1 mcause=0x9 mepc=0x800004c6 expected=0x800004c6 medeleg_restored=0x0
checksum=0xeb198f5eab8207a9
RESULT: PASS
done
```

- Checks: 10, mismatches: 0, checksum `0xeb198f5eab8207a9`
  (FNV-1a over the nine verdict values: S-mode trap count,
  `scause`, `sepc`, M-mode trap count, restore `mcause`,
  `medeleg` write readback, restore readback, U-ecall address,
  restore-ecall address).
- All three runs produced byte-identical program output (the
  `terminating on signal 15` line in the logs is the `timeout`
  harness killing QEMU after the hart parked, not program
  output). Every compared value is a link-time constant or a
  hardware CSR readback; nothing in the checksummed output
  depends on host timing.
- Environment: QEMU 8.2.2.

## What was not verified

- Behavior on real hardware: the delegation logic is defined by
  the privileged specification and measured here under QEMU
  8.2.2 TCG only.
- `medeleg` bit 8 combined with other delegated causes, and
  delegation of U-mode ecalls while `mideleg`-routed interrupts
  are pending: out of scope for this module by design (one
  mechanism under test).
