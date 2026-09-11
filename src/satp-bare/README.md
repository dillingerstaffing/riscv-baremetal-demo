# satp-bare

`satp-bare.elf`: satp MODE=Bare write/readback with a nonzero PPN in
S-mode, plus the physical-access check that Bare means no address
translation. M-mode writes a canary to a scratch physical word,
drops to S-mode, writes `satp` with MODE=Bare and a nonzero PPN,
and reads it back (the MODE field must read 0). While `satp` holds
that Bare+PPN value, an S-mode load of the scratch word must return
the M-mode canary, and an S-mode store/loadback must round-trip
(the effective address is the physical address itself). See
`bare_main.c` for the run; `PROOF.md` carries the build log, the
three raw run logs, and the measured numbers.
