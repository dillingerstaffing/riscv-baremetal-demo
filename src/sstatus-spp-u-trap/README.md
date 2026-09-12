# sstatus.SPP record on a delegated U-mode ecall trap

Delegates user ecalls (`medeleg` bit 8) to S-mode, drops to U-mode
via `mret` (MPP=0), issues one ecall, and checks the S-mode handler
records `sstatus.SPP`=0 with `scause`=8 and zero M-mode traps. The
complement of `src/sstatus-spp/` (the SPP=1 case).

Verification record: [PROOF.md](PROOF.md)
