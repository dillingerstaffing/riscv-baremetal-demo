// tw_mtrap.S: M-mode trap entry for the mstatus-tw-trap module.
//
// A correct run takes no M-mode trap at all: the only exception
// the experiment produces is the S-mode illegal-instruction trap
// from wfi, which medeleg delivers to S-mode, and no M-mode ecall
// is issued. This handler therefore parks the hart on any entry;
// the bench harness runs QEMU under timeout, so an unexpected
// M-mode trap is observable as exit status 124 rather than a
// silent divergence.

    .section .text
    .option push
    .option norvc
    .global tw_mtrap_entry
    .align 2                      // 4-byte aligned: mtvec MODE=00 needs base[1:0] clear
tw_mtrap_entry:
tw_mpark:
    wfi
    j tw_mpark
    .option pop
