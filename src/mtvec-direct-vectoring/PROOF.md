<!-- PROOF-HEADER
Checks: 13
Mismatches: 0
Checksum: 0x3ae8af5dbfec417
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mtvec direct-mode trap vectoring

## What was built

`src/mtvec-direct-vectoring/`: a bare-metal RV64 module that tests
the vectoring MODE of `mtvec` itself. With `mtvec` MODE=00 (Direct),
every trap must enter at the single BASE address, whatever the
cause, with `mcause` carrying the distinguishing information. Two
synchronous traps from two labeled sites exercise this: an illegal
32-bit instruction word (expects `mcause` = 2) and an M-mode `ecall`
(expects `mcause` = 11). This is distinct from `src/mtvec-vectored`
(MODE=1 vectored dispatch), from `src/mtvec-mode0-direct` (direct
mode with an ecall plus a timer interrupt), and from the delegation
modules (which test `medeleg`/`mideleg` bits); the mechanism under
test here is the direct vectoring mode and that two different
synchronous causes both land at BASE.

- `dvm_trap.S`: M-mode trap entry. Swaps t0 through `mscratch`,
  parks the interrupted t1/t2 in save slots, then dispatches on
  `mcause`: code 2 and code 11 each record the exact link-time
  address of the entry that actually ran (the landing address the
  hardware chose), `mcause`, and `mepc` into their own slots and
  bump their own counter; any other cause bumps an unexpected
  counter and records its `mcause`. Every path resumes at
  `mepc+4` (both faulting sites are 4-byte instructions), restores
  the interrupted t0/t1/t2, and returns with `mret`.
- `dvm_main.c`: installs direct-mode `mtvec` and `mscratch`, reads
  `mtvec` back and requires readback == written value with MODE
  bits 00 and BASE == the handler entry. Fires the two traps, then
  requires: each per-cause counter is 1, no unexpected trap fired,
  the illegal trap carried `mcause` = 2 with `mepc` == the illegal
  site, the ecall carried `mcause` = 11 with `mepc` == the ecall
  site, and both recorded landings equal BASE (and each other).
  Trap-site addresses come from in-asm numeric local labels inside
  the faulting asm blocks (a plain C `&&label` at -O2 is not pinned
  to the instruction it labels; the assembler resolves its own
  label exactly). A failed check prints `FAIL` and flips the
  verdict; `RESULT: PASS` is printed only when all 13 checks held.
  On PASS it writes the virt test-device finisher word 0x5555
  (QEMU exits 0); on FAIL it parks the hart in a `wfi` loop.
- `build.sh`: builds `mtvec-direct-vectoring.elf` at the repo root
  with the same flags as the repo Makefile (this module was not
  added to the root Makefile in this commit; it builds standalone).
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three raw QEMU run logs.

Build: `sh src/mtvec-direct-vectoring/build.sh` (from the repo root).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel mtvec-direct-vectoring.elf`
under `timeout`.

Toolchain: riscv64-unknown-elf-gcc 13.2.0, QEMU 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`).

## Configuration under test

- Hart: mhartid = 0, single hart, M-mode (QEMU boots the ELF
  straight into M-mode with `-bios none`).
- No interrupts enabled (mstatus.MIE left at its reset value of 0),
  no S-mode, no PLIC or CLINT involvement. The only traps the run
  can produce are the two synchronous ones it provokes on purpose.

## Measured results (QEMU 8.2.2, `virt`, all three runs byte-identical)

| quantity | run 1 | run 2 | run 3 |
|---|---|---|---|
| `mtvec` written | `0x800001c8` | identical | identical |
| `mtvec` readback | `0x800001c8` | identical | identical |
| illegal trap: count / mcause / mepc / landing | 1 / `0x2` / `0x80000318` / `0x800001c8` | identical | identical |
| ecall trap: count / mcause / mepc / landing | 1 / `0xb` / `0x80000324` / `0x800001c8` | identical | identical |
| trap sites: illegal / ecall | `0x80000318` / `0x80000324` | identical | identical |
| unexpected traps | 0 | identical | identical |
| FNV-1a checksum of published values | `0x3ae8af5dbfec417` | identical | identical |
| verdict | PASS (QEMU exit 0) | identical | identical |

Decoded:

- `mtvec` = `0x800001c8`: BASE `0x800001c8`, MODE 00 (direct).
  Readback equals the written value; the mode bits read 0 and the
  base equals the link-time address of the single trap entry
  `dvm_trap_entry`.
- Illegal-instruction trap: count 1, `mcause` = `0x2`, `mepc` =
  `0x80000318` = the illegal site address captured by the in-asm
  label, landing `0x800001c8` = BASE. The all-zero word trapped
  as an illegal instruction on this QEMU; that is the measured
  `mcause`, asserted by the program's own check.
- M-mode ecall: count 1, `mcause` = `0xb`, `mepc` = `0x80000324`
  = the ecall site, landing `0x800001c8` = BASE.
- Both traps recorded the same landing address, equal to BASE.
  The landing is the handler's own entry address, resolved by the
  assembler inside the trap that actually ran, so a mis-vectored
  trap would have recorded a different address and failed the
  check. `mepc` + 4 resume worked: the ecall trap fired after the
  illegal trap, proving execution continued past the first site.
- The FNV-1a 64-bit checksum covers the `mtvec` readback, both
  landings, both `mcause` values, both `mepc` values, and both
  per-cause counts; it is identical across all three runs.

## Raw QEMU output

`bench-logs/run1.log` (run2 and run3 are byte-for-byte identical;
verified with `cmp`):

```
mtvec-direct-vectoring: direct-mode trap landing test
mtvec: written=0x800001c8 readback=0x800001c8
illegal: count=1 mcause=0x2 mepc=0x80000318 landing=0x800001c8
ecall: count=1 mcause=0xb mepc=0x80000324 landing=0x800001c8
sites: illegal=0x80000318 ecall=0x80000324
unexpected: count=0 mcause=0x0
checksum: fnv1a64=0x3ae8af5dbfec417
RESULT: PASS
```

Build log (bench-logs/build.log):

```
built mtvec-direct-vectoring.elf
```

(the linker emitted its usual `LOAD segment with RWX permissions`
warning for this repo's link.ld; build exit 0.)

## Limits

- Emulator, not silicon: the measured behavior is QEMU 8.2.2's
  model of the `virt` board. On real hardware, direct-mode
  vectoring is defined by the privileged spec; this run measures
  QEMU's model of it.
- Only two synchronous causes are exercised (illegal instruction
  and M-mode ecall). Interrupts are not tested here; vectored
  interrupt dispatch is the subject of `src/mtvec-vectored`.
- The all-zero word traps as an illegal instruction (`mcause` = 2)
  on this QEMU; the run asserts the measured value rather than
  assuming the decode.
