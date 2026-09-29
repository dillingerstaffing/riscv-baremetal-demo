<!-- PROOF-HEADER
Checks: 82
Mismatches: 0
Checksum: 0x30523dc5b14164bb
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: sfence.vma rs2-scoped (ASID-scoped) invalidation (backlog item "riscv sfence-vma-rs2-asid-scoped")

## What was built

`src/sfence-vma-rs2-scoped/`: a bare-metal RISC-V program that
maps one virtual address under two ASIDs to two different
physical pages, remaps both leaves with no fence, then issues
`sfence.vma` naming ASID 1 only, and records whether ASID 2's
cached translation survived. M-mode builds two minimal Sv39
page tables by hand: `root1` (ASID 1) has
`root1[1]` -> `l1a[0]` -> `l0a[0]` mapping `VA = 0x40000000` to
the canary-A page, and `root2` (ASID 2) has
`root2[1]` -> `l1b[0]` -> `l0b[0]` mapping the SAME VA to the
canary-B page. Both roots share `root[2]` -> `l1_id[0]` ->
`l0_id[]`, which identity-maps the program window
`[0x80000000, 0x80080000)` with R|W|X. One PMP NAPOT entry grants
S-mode R|W|X over the whole address space; Sv39 is enabled via
`satp` + `sfence.vma` before the drop to S-mode. The A/D bits
are pre-set on every leaf so no access-fault trap fires to
update them (`menvcfg.ADUE` is 0 on this QEMU). Exactly one
mechanism is under test: whether `sfence.vma` with `rs1 = x0`
and `rs2 = 1` invalidates only ASID 1's translations.

- `srs2_trap.S`: M-mode trap entry (records `mcause`/`mepc`/
  `mstatus`/`mtval` in `m_regs`; the only M-mode traps possible
  are the five phase-return `ecall`s, so the handler loads the
  armed continuation from its save area into `mepc`, sets
  `mstatus.MPP` to M-mode, and `mret`s). The S-mode entry counts
  and parks: no S-mode trap is expected at any point, so any
  arrival is a loud failure, and a faulting phase load can never
  be mistaken for a measured readback.
- `srs2_main.c`: setup, the five S-mode payloads, the four
  M-mode mid phases, and the report.

## Sequence

Phase A runs in S-mode with `satp` = root1/ASID1 and the leaf
mapping VA to the canary-A page: the load must return canary A,
populating the hart's translation cache for the (ASID 1, VA)
entry. The payload `ecall`s back to M-mode. M-mode
(`mid_to_B`) snapshots the trap record, installs root2/ASID2
with a full fence, and drops to S-mode. Phase B reads VA under
ASID 2: expect canary B, populating the (ASID 2, VA) entry. The
payload `ecall`s back. M-mode (`mid_to_C`) snapshots the trap
record, rewrites both leaf PTEs (root1 leaf -> the canary-C
page, root2 leaf -> the canary-D page) with deliberately NO
fence and NO satp write, so `satp` still selects root2/ASID2,
records the rewritten PTEs, and drops to S-mode. Phase C reads
VA under ASID 2 with `satp` untouched since phase B: expect
stale canary B, the cached-stale baseline proving the leaf
rewrite alone invalidates nothing. The payload `ecall`s back.
M-mode (`mid_to_D`) snapshots the trap record and issues the
one fence this module is about: `sfence.vma` with `rs1 = x0`
and `rs2 = 1` (ASID 1 only; the disassembly shows `li a4,1`
immediately before `sfence.vma zero,a4`, i.e. x0 in the rs1
field and the ASID value 1 in the rs2 operand). It drops to
S-mode. Phase D reads VA under ASID 2, still with `satp`
untouched since phase B, and records the verdict. The payload
`ecall`s back. M-mode (`mid_to_E`) snapshots the trap record,
reinstalls root1/ASID1 with a full fence, and drops to S-mode.
Phase E reads VA under ASID 1: expect fresh canary C. The
report snapshots the final trap record, restores `satp` to Bare
and the PMP entry to 0, prints every measured value, runs the
checks, takes a 2M-iteration quiet window, and prints the
verdict.

## Results (QEMU 8.2.2, 3 runs byte-identical, md5 6ba5c7c9054332344d5c09802ba12723)

- `satpA` readback `0x800010000008000f` (MODE=8 Sv39, ASID=1,
  root1 PPN), `satpB` readback `0x800020000008000c` (MODE=8,
  ASID=2, root2 PPN), `satpE` readback `0x800010000008000f`
  (MODE=8, ASID=1); `pte1_before = 0x20001cc7` (ASID1 leaf ->
  PA1), `pte2_before = 0x200018c7` (ASID2 leaf -> PA2),
  `pte1_after = 0x200014c7` (ASID1 leaf -> PA3),
  `pte2_after = 0x200010c7` (ASID2 leaf -> PA4): both rewrites
  landed in memory before the scoped fence.
- Phase A readback `0xaaaaaaaaaaaaaaaa` (A); phase B readback
  `0xbbbbbbbbbbbbbbbb` (B): both (ASID, VA) entries cached.
- Phase C readback `0xbbbbbbbbbbbbbbbb` (stale B): the rewrite
  with no fence invalidated nothing, baseline intact.
- Phase D readback `0xdddddddddddddddd` (fresh D): the
  ASID-1-scoped fence did **not** scope the invalidation on this
  implementation; ASID 2's cached translation was dropped too.
- Phase E readback `0xcccccccccccccccc` (fresh C): ASID 1 reads
  its remapped leaf after the reinstall, sanity confirmation.
- Canaries intact (`s1`..`s4` hold A..D), all four page-end
  sentinels still zero (no side-effect writes).
- `m_traps = 5`, each with `mcause = 0x9` and `mepc` exactly at
  its phase's `ecall` site; `s_traps = 0`; quiet window moved
  neither counter.
- 82 checks, 0 mismatches, FNV-1a `0x30523dc5b14164bb`
  byte-identical across 3 QEMU 8.2.2 runs, QEMU exit 0 on all
  three, Verdict PASS.

## Why this is the real mechanism

The backlog premise was that phase D would read stale canary B
because the fence named only ASID 1. The measurement says
otherwise: phase D reads fresh canary D. This cannot be a test
bug. The disassembly confirms the fence is exactly
`sfence.vma` with rs1 = x0 and the rs2 operand holding ASID 1
(`li a4,1` then `sfence.vma zero,a4` at 0x80000422-24; the only
other `sfence.vma` instances in the binary are the full fences
inside `set_satp`). The PTE readbacks confirm both rewrites
landed in memory before the fence. Phase C is the control that
rules out a missing cache entry: with no fence at all, the same
harness under the same satp reads stale B, so phase D's fresh
readback is the ASID-scoped fence over-invalidating, not a
cache miss. The conclusion is measured, not assumed: on QEMU
8.2.2, `sfence.vma` with `rs1 = x0, rs2 = 1` behaves as a full
local TLB flush, matching the sibling module
`src/sfence-vma-rs1-scoped` (which measured the same
over-invalidation for the rs1 operand) and the independent
observation in the Grinch OS project that QEMU treats any
`sfence.vma` as a full local TLB flush (lfd/grinch commit
`ccccd68`). Over-invalidation is still correct; a guest relying
on ASID scoping for performance simply cannot get it on this
implementation.

Design note: phases C and D deliberately read under ASID 2
with `satp` untouched since phase B, so the stale baseline in C
and the phase-D verdict are attributable to the fence alone,
never to a satp write. There is no ASID-1 stale phase for the
same reason: re-reading under ASID 1 would require a satp
write, which is itself an invalidation event, so it cannot serve
as a no-fence baseline.

## Build log

```
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sfence-vma-rs2-scoped/srs2_trap.S -o src/sfence-vma-rs2-scoped/srs2_trap.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/sfence-vma-rs2-scoped/srs2_main.c -o src/sfence-vma-rs2-scoped/srs2_main.o
riscv64-unknown-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o sfence-vma-rs2-scoped.elf src/boot.o src/uart.o src/sfence-vma-rs2-scoped/srs2_trap.o src/sfence-vma-rs2-scoped/srs2_main.o
```

The C file additionally compiles clean under `-Werror`
(verified separately). Toolchain: Ubuntu
`gcc-riscv64-unknown-elf` 13.2.0
(`~/workspace/toolchains/ubuntu-rv64`). QEMU: 8.2.2
(`~/workspace/qemu/usr/bin/qemu-system-riscv64`). PASS writes
`0x5555` to the virt test-device finisher, so QEMU exits 0; on
FAIL the hart parks.

## Run

```
make sfence-vma-rs2-scoped.elf
make run-sfence-vma-rs2-scoped QEMU=~/workspace/qemu/usr/bin/qemu-system-riscv64
```

(LD_LIBRARY_PATH must cover `~/workspace/qemu/usr/lib/x86_64-linux-gnu`
and `~/workspace/qemu/lib/x86_64-linux-gnu`.)

On a passing run QEMU exits 0 after `RESULT: PASS (checks=82)`.
