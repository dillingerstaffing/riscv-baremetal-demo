<!-- PROOF-HEADER
Checks: 39
Mismatches: 0
Checksum: 0x1cf84d380d3383e1
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: PMP NAPOT match-behavior test

## What was built

`src/pmp-napot-match/`: a bare-metal M-mode RISC-V program that
programs PMP entry 0 as an unlocked NAPOT deny entry
(`pmpaddr0 = 0x200009ff`, `pmpcfg0` byte `0x18`: A = NAPOT, L = 0,
R = W = X = 0) over the 4 KiB region `[0x80002000, 0x80003000)`,
and proves by boundary probes that an access inside the region
traps with the access-fault cause while an access one word outside
the region does not match the entry at all and completes with no
trap. Three files, sharing only `src/boot.S` and `src/uart.c` with
the other demos.

- `pnm_main.c`: UART bring-up, controls, PMP programming of two
  entries, the seven boundary probes, a quiet window, boot-state
  restore, and the PASS/FAIL verdict. Each probe is a single
  inline-asm block so the instruction layout is exact (see below).
- `pnm_trap.S`: minimal M-mode trap entry. mscratch points at the
  8-word `pnm_save` array; on entry it swaps t0, clears MPRV,
  records mcause/mepc/mtval, bumps the total-trap counter, loads
  mepc from the resume address the test stored, flags the trap
  seen, restores t0/t1, and returns with mret.
- `PROOF.md` (this file), `bench-logs/` with the build log and
  three byte-identical run logs.

## Why two PMP entries

An unlocked entry is not checked against M-mode accesses, so the
probes run with `mstatus.MPRV = 1` and `MPP = S`: each load/store
is then privilege-checked as an S-mode access, which the unlocked
deny entry does refuse. (That MPRV makes M-mode data accesses
honor MPP for PMP checks was verified on this QEMU by the
`mstatus-mprv-load` module.)

The privileged spec denies an S-mode access that matches no PMP
entry whenever at least one entry is programmed. A first version
of this module programmed only entry 0 and probed the outside
addresses as S-mode: they trapped, not because entry 0 matched
them, but because the default-deny rule for unmatched S-mode
accesses fired. Entry 1 (`pmpaddr1 = 0x200007ff`, a 16 KiB NAPOT
at `0x80000000` with R = W = X = 1, unlocked) explicitly allows
the outside probe addresses, so the only thing that can trap an
outside probe is entry 0 matching it, which must not happen.
`pmpcfg0` is therefore `0x1f18`: entry 1 = `0x1f` (NAPOT allow),
entry 0 = `0x18` (NAPOT deny). Entry 0 has the lower index, so
inside the deny region its no-permission verdict wins by the
lowest-match-wins priority.

## Controls (what the fault is compared against)

1. Before programming, all four probe addresses are written with
   4-byte canaries in M-mode and read back: proves the probed
   addresses are good RAM, so the later faults come from the PMP
   check and not from a bad address. The canary stores match the
   probe width (`lw`/`sw`), so no probe word overlaps its
   neighbor. No-trap probes verify a real value (the canary), not
   merely the absence of a trap.
2. A layout guard runs before programming: `pnm_save` (the trap
   handler's save area) must sit wholly below `0x80002000`, and
   the live stack pointer must be more than 1 KiB above
   `0x80003000`, so neither the handler nor the C stack can touch
   the scratch region. Both values are run-time constants,
   identical every run (`pnm_save=0x800012d0 sp=0x80005280`).
3. After the probes, a quiet window of 10000 ordinary M-mode
   `mstatus` reads with the entries still programmed must show 0
   new traps, proving the entries disturb nothing but the probed
   addresses.

## Results (three QEMU runs, raw logs in `bench-logs/run[123].log`)

All three runs byte-identical, exit code 0.

| probe | seen | mcause | mepc | mtval | resume-4 | value |
|---|---|---|---|---|---|---|
| t1a lw @0x80002000 (first word inside) | 1 | 0x5 | 0x8000030c | 0x80002000 | 0x8000030c | poison kept |
| t1b lw @0x80002ffc (last word inside) | 1 | 0x5 | 0x8000030c | 0x80002ffc | 0x8000030c | poison kept |
| t2a lw @0x80001ffc (one word below) | 0 | - | - | - | - | 0x6c0de0ca |
| t2b lw @0x80003000 (first word above) | 0 | - | - | - | - | 0x6c0deafe |
| t3a sw @0x80002000 (store inside) | 1 | 0x7 | 0x800004c0 | 0x80002000 | 0x800004c0 | - |
| t3b sw @0x80001ffc (store below) | 0 | - | - | - | - | round-trip ok |
| t3c sw @0x80003000 (store above) | 0 | - | - | - | - | round-trip ok |

(The mcause/mepc/mtval printed on the no-trap lines are stale
leftovers from the previous trap in `pnm_save`; `seen=0` is the
verdict and only it is checked.)

- `config:` lines confirm `pmpaddr0=0x200009ff`,
  `pmpaddr1=0x200007ff`, `pmpcfg0=0x1f18` read back exactly as
  written.
- `mepc` equals the faulting instruction on every trap. The
  disassembly was inspected: the `lw` sits at `0x8000030c` with
  the resume label at `0x80000310`; the `sw` sits at `0x800004c0`
  with resume at `0x800004c4`. Both are 4-byte instructions
  (`.option norvc`) immediately before the resume point, so the
  faulting access is always at `resume - 4`, and the program
  re-checks `mepc == resume - 4` on every trapped probe.
- `mtval` always equals the faulting data address, matching the
  spec's rule that mtval carries the faulting address for access
  faults.
- Inside loads keep the poison `0xdeadbeefdeadbeef` in the
  destination: a faulting load never writes it.
- Outside loads return the exact canary written before
  programming; outside stores round-trip `0x5a5a5a5a` through a
  plain M-mode load.
- Quiet window: traps `3 -> 3`, no unexpected traps.
- Restore: `pmpaddr0`, `pmpaddr1`, `pmpcfg0` all read back `0x0`
  (the boot values), and `mstatus` is restored exactly.
- `Checks: 39`, `Mismatches: 0`,
  `Checksum: 0x1cf84d380d3383e1`, `Environment: QEMU 8.2.2`,
  `Verdict: PASS`.

## One defect found and fixed during development

Recorded here because it changed what was verified.

The first version programmed only the deny entry and ran every
probe as an S-mode access via MPRV. The inside probes trapped
correctly, but the outside probes trapped too, with `mcause=0x5`
and `mtval` equal to the outside address. Reading QEMU 8.2.2's
`target/riscv/pmp.c` showed why: `pmp_hart_has_privs_default`
denies any S-mode access that matches no entry when at least one
entry is programmed, which is the privileged spec's rule, not a
QEMU bug. The outside traps therefore proved nothing about entry
0's match boundary. The fix was entry 1, the 16 KiB NAPOT allow
entry described above: with the outside addresses explicitly
allowed, an outside trap can now only mean entry 0 matched, and
no outside probe traps. A second, smaller defect in the same
version used 8-byte canary stores at 4-byte-spaced addresses, so
neighboring canaries overlapped; the stores are now 4-byte to
match the probes.

## Build

`bench-logs/build.log` captures the full build: GCC 13.2.0,
`-O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie
-fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany`. The
only diagnostic is the repository's usual RWX LOAD segment
warning from the shared linker script.
