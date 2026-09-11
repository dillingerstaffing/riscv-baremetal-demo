<!-- PROOF-HEADER -->
Checks: 11
Mismatches: 0
Checksum: 0x81dc6b1b81f94783
Environment: QEMU 8.2.2
Verdict: PASS

# Proof: sstatus.SPP record on delegated S-mode ecall traps (backlog item 174)

## What was built

`src/sstatus-spp/`: a bare-metal RISC-V program that checks the one
hardware behavior under test: sstatus.SPP records the privilege mode
the hart was in before a trap. M-mode boots, delegates supervisor
ecalls (medeleg bit 9) to S-mode, installs a direct-mode stvec, opens
the address space to S-mode with one PMP NAPOT entry, sets
sstatus.SPP=1 and mstatus.MPP=1, and srets into an S-mode payload. The
payload issues two ecalls; each trap must enter the S-mode handler
with sstatus.SPP=1 and scause=9. The handler records SPP, scause and
sepc per trap, advances sepc by 4, and srets back to S-mode. Four
files, sharing only `src/boot.S` and `src/uart.c` with the other
demos.

- `ssp_trap.S`: S-mode trap entry. Bumps a trap counter, records
  scause/sepc/stval and sstatus.SPP (bit 8) of every trap, copies the
  values into sticky per-trap slots for trap 1 and trap 2, advances
  sepc by 4 past the ecall, restores t0/t1/t2, and returns with
  sret. Any other scause, or a third trap, parks the hart. Also an
  M-mode trap entry that records mcause and parks: no trap may reach
  M-mode during the run.
- `ssp_main.c`: M-mode setup (mtvec/mscratch, medeleg bit 9 with
  readback check, stvec/sscratch with readback check, PMP NAPOT
  R/W/X over the whole address space, SPP=1, MPP=1, sepc at the
  payload, sret) and the S-mode payload (two ecalls, print the
  recorded values, a checksum over them, 11 checks, finisher word
  0x5555 at 0x100000 on PASS for a QEMU exit code of 0, wfi park on
  FAIL).
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make sstatus-spp.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel sstatus-spp.elf`
(or `make run-sstatus-spp`).

Toolchain: riscv64-unknown-elf-gcc 13.2.0, QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart, QEMU boots the ELF straight into
  M-mode with `-bios none` on the `virt` board.
- The program is the only code running; no interrupts armed, no PLIC
  or CLINT involvement. The only traps are the two deliberate
  S-mode ecalls, routed to the S-mode handler by medeleg bit 9.
- Reaching the S-mode handler at all proves the sret dropped to
  S-mode: an M-mode ecall would report mcause 11 and trap to the
  M-mode handler (medeleg delegates only cause 9), which parks the
  hart instead of returning.

## Sequence and controls

1. M-mode: install direct-mode mtvec (parks on any M-mode trap),
   set medeleg bit 9, read medeleg back (must show 0x200).
2. Install direct-mode stvec and sscratch, read stvec back (must
   match the handler address, mode 0).
3. Program one PMP NAPOT entry covering the whole address space,
   R/W/X, so S-mode can fetch and access memory.
4. Set sstatus.SPP=1 and mstatus.MPP=1, point sepc at the S-mode
   payload, sret. Never returns to M-mode.
5. Payload: ecall (trap 1), ecall (trap 2). The handler records SPP
   (sstatus bit 8), scause and sepc per trap, advances sepc by 4,
   srets.
6. Payload prints the recorded values and a checksum over them, then
   runs 11 checks: medeleg bit 9 stuck, stvec address and mode, trap
   count 2, SPP=1 and scause=9 on both traps, equal sepc values (both
   traps come from the one ecall inside trap_once, executed twice),
   the word at the recorded sepc is the ecall encoding 0x00000073,
   and zero M-mode traps.

## Measured results

3-run table (identical on every run, md5 2a35a5905dba2119cfb4f16cbf16f450):

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| `medeleg` readback after setting bit 9 | `0x200` | identical | identical |
| trap1 SPP / scause / sepc | `1` / `0x9` / `0x800002a8` | identical | identical |
| trap2 SPP / scause / sepc | `1` / `0x9` / `0x800002a8` | identical | identical |
| word at trap sepc | `0x73` (ecall) | identical | identical |
| checksum over the six recorded values | `0x81dc6b1b81f94783` | identical | identical |
| M-mode traps | `0` | identical | identical |
| checks passed / failed | 11 / 0 | identical | identical |
| QEMU exit code | 0 | 0 | 0 |
| verdict | PASS | PASS | PASS |

SPP read 1 on both traps and scause read 9 on both traps in all
three runs; the finisher word shut the machine down and QEMU exited
0 each time.

## Raw QEMU output

### Run 1 (bench-logs/ssp-run1.log)

```
sstatus-spp: sstatus.SPP record on delegated S-mode ecall traps
deleg: medeleg=0x200
trap1: spp=1 scause=0x9 sepc=0x800002a8
trap2: spp=1 scause=0x9 sepc=0x800002a8
cksum=0x81dc6b1b81f94783
m-traps=0
RESULT: PASS
done
```

### Run 2 (bench-logs/ssp-run2.log)

```
sstatus-spp: sstatus.SPP record on delegated S-mode ecall traps
deleg: medeleg=0x200
trap1: spp=1 scause=0x9 sepc=0x800002a8
trap2: spp=1 scause=0x9 sepc=0x800002a8
cksum=0x81dc6b1b81f94783
m-traps=0
RESULT: PASS
done
```

### Run 3 (bench-logs/ssp-run3.log)

```
sstatus-spp: sstatus.SPP record on delegated S-mode ecall traps
deleg: medeleg=0x200
trap1: spp=1 scause=0x9 sepc=0x800002a8
trap2: spp=1 scause=0x9 sepc=0x800002a8
cksum=0x81dc6b1b81f94783
m-traps=0
RESULT: PASS
done
```

## Limits

- Emulator, not silicon: the delegation and SPP behavior measured
  are QEMU 8.2.2's model of the `virt` board. The privileged
  specification defines SPP as the previous privilege mode, but a
  physical core's reset values (medeleg, PMP defaults) may differ.
- Only the synchronous S-mode ecall path was exercised. Interrupts,
  page faults, and traps from U-mode were not part of this module;
  SPP behavior on those paths is a separate measurement.
- The module proves the record-and-return behavior for two traps
  from one static ecall site; it does not measure trap latency or
  handler overhead.
