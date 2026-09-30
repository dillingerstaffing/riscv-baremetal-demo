# mhartid-readonly

Verifies the read-only invariant of the `mhartid` CSR in M-mode on the QEMU `virt` board: a `csrw` of all-ones and a `csrw` of zero both raise an illegal-instruction exception (recorded by a counting trap handler that resumes past the faulting instruction) and neither changes the readback, which stays `0x0`. See [PROOF.md](PROOF.md) for the measured results.
