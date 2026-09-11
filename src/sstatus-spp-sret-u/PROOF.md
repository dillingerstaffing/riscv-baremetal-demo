<!-- PROOF-HEADER -->
Checks: 10
Mismatches: 0
Checksum: 0x16e5b2ab913eb291
Environment: QEMU 8.2.2
Verdict: PASS

# Proof: sret with sstatus.SPP=0 drops to U-mode (backlog item: riscv sstatus-spp-sret-u)

## What was built

`src/sstatus-spp-sret-u/`: a bare-metal RISC-V program that checks
the one hardware behavior under test: sstatus.SPP selects the
privilege mode after sret, so sret with SPP=0 must drop to U-mode.
M-mode boots, leaves medeleg at 0 (the illegal-instruction trap must
stay in M-mode), installs a direct-mode mtvec and a counting
direct-mode stvec (control: must see 0 traps), opens the whole
address space to U-mode with one PMP NAPOT entry R/W/X, clears
sstatus.SPP, reads it back (0), points sepc at a U-mode landing pad,
and srets. Never returns to M-mode. The landing pad records the
address of its privileged read (`csrr sstatus`) and executes it;
U-mode may not read sstatus, so the hart traps to M-mode with
mcause=2. The M-mode handler records mcause/mepc/mtval/mstatus,
bumps the trap counter, and jumps to a C continuation that prints
the recorded values, a checksum over them, runs 10 checks, and on
PASS writes the finisher word 0x5555 at 0x100000 for a QEMU exit code
of 0; on FAIL it parks the hart in a wfi loop. Four files, sharing
only `src/boot.S` and `src/uart.c` with the other demos.

- `spu_trap.S`: M-mode trap entry (records mcause/mepc/mtval/
  mstatus, jumps to the C continuation), S-mode trap entry (counts
  traps, parks: off-script), and the U-mode landing pad (records the
  read address with an in-asm numeric local label, then the
  privileged `csrr sstatus`; any fall-through parks, since reaching
  it would mean the read did not trap).
- `spu_main.c`: M-mode setup (mtvec/mscratch, medeleg=0 with
  readback check, stvec/sscratch with readback check, PMP NAPOT
  R/W/X, SPP=0 with readback check, sepc at the landing pad, sret)
  and the M-mode continuation (prints the trap record, checksum, 10
  checks, finisher on PASS).
- `PROOF.md` (this file), `bench-logs/` with the build log and three
  raw QEMU run logs.

Build: `make sstatus-spp-sret-u.elf` (added to `all` in the Makefile).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel sstatus-spp-sret-u.elf`
(or `make run-sstatus-spp-sret-u`).

Toolchain: riscv64-unknown-elf-gcc 15.2.0 (xPack, via
`~/workspace/toolchains/compat-bin`), QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart, QEMU boots the ELF straight into
  M-mode with `-bios none` on the `virt` board.
- The program is the only code running; no interrupts armed, no PLIC
  or CLINT involvement. The only trap is the one deliberate
  illegal-instruction trap from U-mode, kept in M-mode by
  medeleg=0.
- The proof that the hart was in U-mode is threefold: mcause=2
  (illegal instruction on the privileged CSR read), mepc exactly at
  the read site, and the trapped mstatus.MPP field reading 0
  (U-mode). Had the sret not dropped privilege, the csrr would have
  succeeded in M-mode and no trap would exist: the landing pad would
  fall through to its park loop and the run would time out instead
  of printing RESULT.

## Sequence and controls

1. M-mode: install direct-mode mtvec (records the trap, jumps to
   the continuation) and direct-mode stvec (counts S-mode traps,
   parks: off-script).
2. Write medeleg=0, read back (must be 0x0, so the illegal-
   instruction trap is not delegated).
3. Program one PMP NAPOT entry covering the whole address space,
   R/W/X, so U-mode can fetch and access memory (without it,
   U-mode default-deny raises an instruction access fault on the
   first fetch).
4. Clear sstatus.SPP, read sstatus back (SPP field must be 0),
   point sepc at the U-mode landing pad, sret. Never returns to
   M-mode.
5. Landing pad: record the address of the `csrr sstatus`, execute
   it. The trap lands in the M-mode handler with mcause=2; the
   handler records mcause/mepc/mtval/mstatus and jumps to the
   continuation.
6. Continuation prints the trap record and runs 10 checks: medeleg
   readback 0, stvec address and direct mode, SPP readback 0,
   M-mode trap count 1, mcause 2, mepc at the recorded read site,
   trapped mstatus.MPP 0, mtval equal to the word at mepc, S-mode
   trap count 0.

## Measured results

3-run table (identical on every run, md5 b695cf9c1e9d9f372aac034db7c8466b):

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| `medeleg` readback after writing 0 | `0x0` | identical | identical |
| `sstatus.SPP` readback after clearing | `0` | identical | identical |
| M-mode trap count | `1` | identical | identical |
| `mcause` | `0x2` (illegal instruction) | identical | identical |
| `mepc` | `0x8000023c` | identical | identical |
| trapped `mstatus.MPP` | `0` (U-mode) | identical | identical |
| `mtval` | `0x100022f3` (the trapped `csrr` encoding) | identical | identical |
| S-mode traps | `0` | identical | identical |
| checksum over the measured values | `0x16e5b2ab913eb291` | identical | identical |
| checks passed / failed | 10 / 0 | identical | identical |
| QEMU exit code | 0 | 0 | 0 |
| verdict | PASS | PASS | PASS |

`mcause` read 2, `mepc` matched the recorded read site
`0x8000023c`, and the trapped `mstatus.MPP` read 0 in all three
runs; the finisher word shut the machine down and QEMU exited 0
each time.

Note: `mtval` is not 0 on this QEMU build. QEMU 8.2.2 writes the
faulting instruction word into `mtval` on an illegal-instruction
trap; `0x100022f3` decodes as `csrrs t0, sstatus, zero`, exactly the
trapping read. The check compares `mtval` against the actual word at
`mepc` rather than a guessed constant.

## Raw QEMU output

### Run 1 (bench-logs/spu-run1.log)

```
sstatus-spp-sret-u: sret with SPP=0 drops to U-mode
deleg: medeleg=0x0
spp: readback=0
trap: count=1 mcause=0x2 mepc=0x8000023c mtval=0x100022f3 mpp=0
site: expected=0x8000023c
cksum=0x16e5b2ab913eb291
s-traps=0
RESULT: PASS
done
```

### Run 2 (bench-logs/spu-run2.log)

```
sstatus-spp-sret-u: sret with SPP=0 drops to U-mode
deleg: medeleg=0x0
spp: readback=0
trap: count=1 mcause=0x2 mepc=0x8000023c mtval=0x100022f3 mpp=0
site: expected=0x8000023c
cksum=0x16e5b2ab913eb291
s-traps=0
RESULT: PASS
done
```

### Run 3 (bench-logs/spu-run3.log)

```
sstatus-spp-sret-u: sret with SPP=0 drops to U-mode
deleg: medeleg=0x0
spp: readback=0
trap: count=1 mcause=0x2 mepc=0x8000023c mtval=0x100022f3 mpp=0
site: expected=0x8000023c
cksum=0x16e5b2ab913eb291
s-traps=0
RESULT: PASS
done
```

## Limits

- Emulator, not silicon: the sret privilege-drop and SPP behavior
  measured are QEMU 8.2.2's model of the `virt` board. The
  privileged specification defines SPP as selecting the post-sret
  privilege, but a physical core's reset values (medeleg, PMP
  defaults) may differ.
- Only the synchronous U-mode illegal-instruction path was
  exercised, with interrupts disabled and medeleg=0. Delegated
  variants of the trap, and the SPP=1 return-to-S-mode path, are
  separate measurements (the latter is `src/sstatus-spp/`).
- The module proves the drop for one trap from one static read
  site; it does not measure trap latency or handler overhead.
