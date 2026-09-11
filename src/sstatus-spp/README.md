# sstatus.SPP record on delegated S-mode ecall traps (backlog item 174)

Delegates supervisor ecalls (`medeleg` bit 9) to S-mode, drops to
S-mode via `sret`, and checks that each of the two S-mode ecall traps
enters the handler with `sstatus.SPP` recording S-mode and
`scause` reporting the S-mode environment call.

Verification record: [PROOF.md](PROOF.md)
