// mnr_main.c: mret-no-restore check (backlog item "riscv mret-no-restore").
//
// Mechanism under test: a trap handler that writes new values into
// registers and returns with mret without restoring them leaves its
// clobbers observable to the pre-trap caller. The program loads eight
// distinct sentinel values into a0..a7, records them, issues exactly
// one ecall, and the M-mode handler (mnr_trap.S) records
// mcause/mepc/mtval, writes a different set of eight sentinels into
// a0..a7, advances mepc past the ecall, and mrets with no register
// restore. After mret the pre-trap code reads a0..a7 back and the
// program asserts every register changed from its pre-trap value and
// exactly matches the handler's written value. The pre/post register
// dumps are the ground truth; RESULT: PASS prints only when all eight
// pairs verify, plus mcause == 11 (M-mode ecall), mtval == 0, the
// instruction word at the recorded mepc == 0x00000073 (ecall), and
// exactly one trap taken.
//
// The sentinel load, the pre-trap record, the ecall, and the post-trap
// readback live in a single volatile asm block so the compiler emits
// nothing between them and cannot touch a0..a7 (all eight are in the
// clobber list) across the trap boundary; pre/post values travel to C
// only through the memory stores inside the block. The block assembles
// with .option norvc so the ecall is exactly 4 bytes, matching the
// handler's mepc += 4.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.

#include "../uart.h"
#include "mnr_vals.h"

#define CLINT_MTIME   0x0200bff8UL  // 64-bit mtime, UART-drain timebase

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

#define MCAUSE_M_ECALL 11UL
#define INSN_ECALL     0x00000073UL

// Trap scratch: mcause, mepc, mtval as recorded by the handler, then
// the trap counter. mscratch points here; boot.S clears BSS so the
// counter starts at 0.
static volatile unsigned long mnr_regs[4];

static const unsigned long pre_vals[8] = {
    PRE_A0, PRE_A1, PRE_A2, PRE_A3, PRE_A4, PRE_A5, PRE_A6, PRE_A7
};
static const unsigned long post_vals[8] = {
    POST_A0, POST_A1, POST_A2, POST_A3, POST_A4, POST_A5, POST_A6, POST_A7
};

extern void mnr_trap_entry(void);

static unsigned long read_mtime(void) {
    return *(volatile unsigned long *)CLINT_MTIME;
}

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

int main(void) {
    unsigned long pre[8], post[8];
    unsigned long mcause, mepc, mtval, count, insn;
    int i, j;

    uart_init();
    uart_puts("mret-no-restore: handler clobbers a0..a7 with no restore\n");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the scratch area. Interrupts stay off (mie must read 0 at
    // boot and mstatus.MIE is cleared explicitly) so the ecall is the
    // only trap the run can take.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mnr_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mnr_regs));
    __asm__ volatile("csrci mstatus, 8");
    {
        unsigned long tv, mie;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        __asm__ volatile("csrr %0, mie" : "=r"(mie));
        uart_puts("setup: mtvec=");
        uart_put_hex(tv);
        uart_puts(" mie=");
        uart_put_hex(mie);
        uart_puts("\n");
        check((tv & ~3UL) == (unsigned long)mnr_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
        check(mie == 0, "mie nonzero at boot");
    }

    // Static disjointness: the two sentinel sets must not overlap,
    // or "changed" would be uncheckable.
    for (i = 0; i < 8; i++)
        for (j = 0; j < 8; j++)
            check(pre_vals[i] != post_vals[j], "sentinel sets overlap");

    // One volatile block: load the pre-trap sentinels into a0..a7,
    // record them to memory, ecall (exactly one synchronous trap),
    // then read a0..a7 back to memory after mret returns. The "i"
    // operands carry the sentinel immediates from mnr_vals.h; the
    // clobber list keeps the compiler's hands off a0..a7 for the
    // whole block.
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "li a0, %[v0]\n\t"
        "li a1, %[v1]\n\t"
        "li a2, %[v2]\n\t"
        "li a3, %[v3]\n\t"
        "li a4, %[v4]\n\t"
        "li a5, %[v5]\n\t"
        "li a6, %[v6]\n\t"
        "li a7, %[v7]\n\t"
        "sd a0, 0*8(%[pre])\n\t"
        "sd a1, 1*8(%[pre])\n\t"
        "sd a2, 2*8(%[pre])\n\t"
        "sd a3, 3*8(%[pre])\n\t"
        "sd a4, 4*8(%[pre])\n\t"
        "sd a5, 5*8(%[pre])\n\t"
        "sd a6, 6*8(%[pre])\n\t"
        "sd a7, 7*8(%[pre])\n\t"
        "ecall\n\t"
        "sd a0, 0*8(%[post])\n\t"
        "sd a1, 1*8(%[post])\n\t"
        "sd a2, 2*8(%[post])\n\t"
        "sd a3, 3*8(%[post])\n\t"
        "sd a4, 4*8(%[post])\n\t"
        "sd a5, 5*8(%[post])\n\t"
        "sd a6, 6*8(%[post])\n\t"
        "sd a7, 7*8(%[post])\n\t"
        ".option pop\n\t"
        : /* no outputs; pre/post travel through the in-block stores */
        : [pre] "r" (pre), [post] "r" (post),
          [v0] "i" (PRE_A0), [v1] "i" (PRE_A1),
          [v2] "i" (PRE_A2), [v3] "i" (PRE_A3),
          [v4] "i" (PRE_A4), [v5] "i" (PRE_A5),
          [v6] "i" (PRE_A6), [v7] "i" (PRE_A7)
        : "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "memory");

    // Trap record from the handler.
    mcause = mnr_regs[0];
    mepc = mnr_regs[1];
    mtval = mnr_regs[2];
    count = mnr_regs[3];
    insn = *(volatile unsigned int *)mepc;
    uart_puts("trap: count=");
    uart_put_dec(count);
    uart_puts(" mcause=");
    uart_put_hex(mcause);
    uart_puts(" mepc=");
    uart_put_hex(mepc);
    uart_puts(" mtval=");
    uart_put_hex(mtval);
    uart_puts(" insn@mepc=");
    uart_put_hex(insn);
    uart_puts("\n");
    check(count == 1, "trap count != 1");
    check(mcause == MCAUSE_M_ECALL, "mcause != 11 (M-mode ecall)");
    check(mtval == 0, "mtval != 0");
    check(insn == INSN_ECALL, "instruction at mepc is not ecall");

    // The ground truth: before/after dump of a0..a7.
    uart_puts("regs: before -> after (handler wrote)\n");
    for (i = 0; i < 8; i++) {
        uart_puts("  a");
        uart_put_dec((unsigned long)i);
        uart_puts(": ");
        uart_put_hex(pre[i]);
        uart_puts(" -> ");
        uart_put_hex(post[i]);
        uart_puts(" (handler ");
        uart_put_hex(post_vals[i]);
        uart_puts(")");
        if (post[i] != post_vals[i] || post[i] == pre[i] ||
            pre[i] != pre_vals[i])
            uart_puts("  MISMATCH");
        uart_puts("\n");
        check(pre[i] == pre_vals[i], "pre-trap record != loaded sentinel");
        check(post[i] == post_vals[i], "post-trap register != handler value");
        check(post[i] != pre[i], "register unchanged across the trap");
    }

    uart_puts("RESULT: ");
    uart_puts(fails == 0 ? "PASS" : "FAIL");
    uart_puts(" (traps=");
    uart_put_dec(count);
    uart_puts(")\n");

    // Let the UART drain before touching the finisher device.
    {
        unsigned long drain = read_mtime();
        while (read_mtime() - drain < 100000UL)
            ;
    }

    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    // FAIL: park the hart without touching the finisher device. The
    // harness runs QEMU under timeout, so a FAIL is observable as
    // the timeout exit status (124) as well as the RESULT line.
    for (;;)
        __asm__ volatile("wfi");
}
