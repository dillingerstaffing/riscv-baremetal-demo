<!-- PROOF-HEADER -->
Checks: 8
Mismatches: 0
Checksum: 0x904390c30350859
Environment: QEMU 8.2.2
Verdict: PASS

# Proof: sstatus.SPP record on a delegated U-mode ecall trap

## What was built

`src/sstatus-spp-u-trap/`: a bare-metal RISC-V program that checks the
one hardware behavior under test: sstatus.SPP records the privilege
mode the hart was in before a trap. M-mode boots, records the boot
medeleg value, delegates user ecalls (medeleg bit 8) to S-mode,
installs a direct-mode stvec, opens the address space to lower modes
with one PMP NAPOT entry, clears sstatus.SPP, sets mstatus.MPP=0, and
mrets into a U-mode payload. The payload issues one ecall; the trap
must enter the S-mode handler with sstatus.SPP=0 and scause=8. The
handler records SPP, scause and sepc, advances sepc by 4, and srets
back to U-mode. Four files, sharing only `src/boot.S` and
`src/uart.c` with the other demos.

- `sppu_trap.S`: S-mode trap entry. Bumps a trap counter, records
  scause/sepc/stval and sstatus.SPP (bit 8), copies the values into
  sticky slots for the one scripted trap, advances sepc by 4 past
  the ecall, restores t0/t1/t2, and returns with sret (SPP=0, so
  control returns to U-mode). Any other scause, or a second trap,
  parks the hart. Also an M-mode trap entry that records mcause and
  parks: no trap may reach M-mode during the run.
- `sppu_main.c`: M-mode setup (mtvec/mscratch, boot medeleg capture,
  medeleg bit 8 with readback check, stvec/sscratch with readback
  check, PMP NAPOT R/W/X over the whole address space, SPP cleared,
  MPP=0, mepc at the U-mode payload, mret) and the U-mode payload
  (one ecall, print the recorded values, a checksum over them,
  8 checks, finisher word 0x5555 at 0x100000 on PASS for a QEMU exit
  code of 0, wfi park on FAIL).
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make sstatus-spp-u-trap.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel sstatus-spp-u-trap.elf`
(or `make run-sstatus-spp-u-trap`).

Toolchain: riscv64-unknown-elf-gcc 13.2.0, QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart, QEMU boots the ELF straight into
  M-mode with `-bios none` on the `virt` board.
- The program is the only code running; no interrupts armed, no PLIC
  or CLINT involvement. The only trap is the one deliberate U-mode
  ecall, routed to the S-mode handler by medeleg bit 8.
- Reaching the S-mode handler at all proves the mret dropped to
  U-mode: an M-mode ecall would report mcause 11 and trap to the
  M-mode handler (medeleg delegates only cause 8), which parks the
  hart instead of returning.
- The S-mode handler returns with sret while sstatus.SPP=0, so the
  payload resumes in U-mode past the ecall; the printed values and
  the finisher word are therefore produced by U-mode code.

## Sequence and controls

1. M-mode: install direct-mode mtvec (parks on any M-mode trap),
   capture boot medeleg (0x0), set medeleg bit 8, read medeleg back
   (must show 0x100).
2. Install direct-mode stvec and sscratch, read stvec back (must
   match the handler address, mode 0).
3. Program one PMP NAPOT entry covering the whole address space,
   R/W/X, so U-mode can fetch and access memory.
4. Clear sstatus.SPP, set mstatus.MPP=0, point mepc at the U-mode
   payload, mret. Never returns to M-mode.
5. Payload: ecall. The handler records SPP (sstatus bit 8), scause
   and sepc, advances sepc by 4, srets to U-mode.
6. Payload prints the recorded values and a checksum over them, then
   runs 8 checks: medeleg bit 8 stuck, stvec address and mode, trap
   count 1, SPP=0 and scause=8, the word at the recorded sepc is the
   ecall encoding 0x00000073, and zero M-mode traps.
7. medeleg is intentionally left with bit 8 set: the scripted run
   never re-enters M-mode (restoring the register would require an
   M-mode trap, which the assertions forbid), and the finisher shuts
   the machine down immediately after the verdict.

## Measured results

3-run table (identical on every run, md5 ad3d08de7952a3693d4f734ca61484fb):

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| boot `medeleg` | `0x0` | identical | identical |
| `medeleg` readback after setting bit 8 | `0x100` | identical | identical |
| trap SPP / scause / sepc | `0` / `0x8` / `0x80000288` | identical | identical |
| word at trap sepc | `0x73` (ecall) | identical | identical |
| checksum over the three recorded values | `0x904390c30350859` | identical | identical |
| M-mode traps | `0` | identical | identical |
| checks passed / failed | 8 / 0 | identical | identical |
| QEMU exit code | 0 | 0 | 0 |
| verdict | PASS | PASS | PASS |

SPP read 0 and scause read 8 in all three runs; objdump confirms
0x80000288 is the `ecall` (word 0x00000073) at the start of
`trap_once`, so the recorded sepc is exactly the trapping
instruction. The finisher word shut the machine down and QEMU exited
0 each time.

## Raw QEMU output

### Run 1 (bench-logs/run1.log)

```
sstatus-spp-u-trap: sstatus.SPP record on delegated U-mode ecall trap
deleg: boot_medeleg=0x0 medeleg=0x100
trap: spp=0 scause=0x8 sepc=0x80000288
cksum=0x904390c30350859
m-traps=0
RESULT: PASS
done
```

### Run 2 (bench-logs/run2.log)

```
sstatus-spp-u-trap: sstatus.SPP record on delegated U-mode ecall trap
deleg: boot_medeleg=0x0 medeleg=0x100
trap: spp=0 scause=0x8 sepc=0x80000288
cksum=0x904390c30350859
m-traps=0
RESULT: PASS
done
```

### Run 3 (bench-logs/run3.log)

```
sstatus-spp-u-trap: sstatus.SPP record on delegated U-mode ecall trap
deleg: boot_medeleg=0x0 medeleg=0x100
trap: spp=0 scause=0x8 sepc=0x80000288
cksum=0x904390c30350859
m-traps=0
RESULT: PASS
done
```

## Limits

- Emulator, not silicon: the delegation and SPP behavior measured
  are QEMU 8.2.2's model of the `virt` board. The privileged
  specification defines SPP as the previous privilege mode, but a
  physical core's reset values (medeleg, PMP defaults) may differ.
- Only the synchronous U-mode ecall path was exercised. Interrupts,
  page faults, and traps from S-mode were not part of this module;
  SPP behavior on those paths is a separate measurement (S-mode
  ecall traps are covered by `src/sstatus-spp/`).
- The module proves the record-and-return behavior for one trap
  from one static ecall site; it does not measure trap latency or
  handler overhead.
