<!-- PROOF-HEADER
Checks: 7
Mismatches: 0
Checksum: 0x3135d0ef80ebdf2f
Environment: QEMU 8.2.2
Verdict: PASS
-->

# PROOF: stval illegal-instruction capture

In S-mode on QEMU 8.2.2 (virt board), the module executes the word
0xffffffff at a labeled site, takes the illegal-instruction trap,
and checks what the hart wrote to stval. Across three runs the
record is byte-identical:

- exactly one S-mode trap, zero M-mode traps
- scause = 0x2 (illegal instruction)
- sepc = address of the executed word (0x8000030e in this build)
- stval = 0xffffffff, equal to the executed encoding
- the S-mode handler skipped the 4-byte word and resumed the
  payload, which ran to PASS and shut the machine down via the
  virt test-device finisher (QEMU exit code 0 on all three runs)

## Delegation

Illegal-instruction delegation is medeleg bit 2. The boot medeleg
readback on this hart is 0x0. The write/readback triple, published
in the run logs: write 0 reads back 0x0; write 0x4 reads back 0x4
(bit 2 admitted, nothing else changed); write 0x4 again reads back
0x4. The trap was routed to S-mode on the final value.

## What the check asserts

The seven checks are: the trap arrived (bounded wait did not
expire), exactly one S-mode trap, scause == 2, sepc == the labeled
word address, stval == the executed word 0xffffffff, no M-mode
trap, and the trap counts unchanged after the quiet window. All
interrupt enables stay clear for the whole run, so no interrupt of
either kind can fire; the only possible trap is the illegal
instruction under test.

## Reproducing

Toolchain: riscv64-unknown-elf-gcc. Build:
`CROSS=<toolchain-prefix> make stval-illegal-capture.elf`
(the captured build log is bench-logs/build.log). Run three
times:
`qemu-system-riscv64 -machine virt -nographic -bios none -kernel stval-illegal-capture.elf`
captured as bench-logs/run1.log, run2.log, run3.log. The three logs
are byte-identical; the checksum above is the FNV-1a 64-bit hash
over the verdict record (medeleg readbacks, trap counts, scause,
sepc, stval) printed by the module itself.
