<!-- PROOF-HEADER
Checks: 8
Mismatches: 0
Checksum: 0x6d35f7bd48b0629d
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: stval environment-call capture

On a U-mode environment-call trap, the hart writes zero to
stval. The privileged specification defines stval content only
for instruction, load, and store address-misaligned and access
and page faults; for environment calls there is no faulting
address or word to report, so the expected recorded value is
zero. Source: the RISC-V privileged architecture specification,
trap handling chapter (the pinned ISA manual used for this run:
https://github.com/riscv/riscv-isa-manual/releases/download/riscv-isa-release-dc8bf2a-2026-06-26/riscv-spec.pdf).

In M-mode on QEMU 8.2.2 (virt board), the module sets medeleg
bit 8 (delegating U-mode environment calls to S-mode), installs
an S-mode direct-mode stvec handler that records scause, sepc,
and stval, and drops to U-mode via sret with sstatus.SPP = 0.
The U-mode payload issues one ecall at a labeled site. Across
three runs the record is byte-identical:

- exactly one S-mode trap, zero M-mode traps
- scause = 0x8 (environment call from U-mode)
- sepc = address of the ecall (0x80000306 in this build)
- stval = 0x0
- the S-mode handler skipped the 4-byte ecall and resumed the
  payload in U-mode, which ran to PASS and shut the machine down
  via the virt test-device finisher (QEMU exit code 0 on all
  three runs)

This is distinct from src/stval-illegal-capture/ (illegal
instruction writes the faulting encoding to stval) and from
src/medeleg-ecall-u-route/ (which routed the U-mode ecall but did
not publish stval).

## Delegation

U-mode environment-call delegation is medeleg bit 8. The boot
medeleg readback on this hart is 0x0. The write/readback triple,
published in the run logs: write 0 reads back 0x0; write 0x100
reads back 0x100 (bit 8 admitted, nothing else changed); write
0x100 again reads back 0x100. The trap was routed to S-mode on
the final value.

## What the check asserts

The eight checks are: medeleg bit 8 admitted cleanly, the trap
arrived (bounded wait did not expire), exactly one S-mode trap,
scause == 8, sepc == the labeled ecall address, stval == 0, no
M-mode trap, and the trap counts unchanged after the quiet
window. All interrupt enables stay clear for the whole run, so
no interrupt of either kind can fire; the only possible trap is
the U-mode ecall under test.

## Limits

These are emulator observations, not silicon measurements: the
run proves what QEMU 8.2.2's virt machine model writes to stval
on a delegated U-mode environment call. A real hart is required
to confirm the behavior on hardware.

## Reproducing

Toolchain: riscv64-unknown-elf-gcc. Build:
`CROSS=<toolchain-prefix> make stval-ecall-capture.elf`
(the captured build log is bench-logs/build.log). Run three
times:
`qemu-system-riscv64 -machine virt -nographic -bios none -kernel stval-ecall-capture.elf`
captured as bench-logs/run1.log, run2.log, run3.log. The three
logs are byte-identical; the checksum above is the FNV-1a 64-bit
hash over the verdict record (medeleg readbacks, trap counts,
scause, sepc, stval) printed by the module itself.
