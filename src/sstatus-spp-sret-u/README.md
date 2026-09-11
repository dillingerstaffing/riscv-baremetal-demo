# sret with sstatus.SPP=0 drops to U-mode (backlog item: riscv sstatus-spp-sret-u)

Clears `sstatus.SPP`, reads it back (0), and `sret`s to a U-mode
landing pad that executes a privileged read (`csrr sstatus`). The
read must trap to M-mode with `mcause=2` (illegal instruction),
`mepc` exactly at the read site, and the trapped `mstatus.MPP`
field reading 0 (U-mode), proving the hart was in U-mode. `medeleg`
stays 0 so the trap is handled in M-mode; a counting S-mode handler
is installed as a control and must see 0 traps.

Verification record: [PROOF.md](PROOF.md)
