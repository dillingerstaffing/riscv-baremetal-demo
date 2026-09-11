// mpnt_main.c: mip pending-bit reflectivity without a trap (backlog
// item 171).
//
// Mechanism under test: mip reflects pending state independently of
// the interrupt enable gate. A pended interrupt with the global gate
// clear shows up in mip while no trap fires. Sequence, all on hart 0
// in M-mode on the QEMU virt board:
//
//   1. Read back mstatus.MIE and mie.MSIE at boot; explicitly clear
//      both (csrc) and read back, so the global gate and the per-
//      interrupt enable are both off for the whole run. Install a
//      direct-mode mtvec handler (mscratch scratch area) that counts
//      entries; the count is the experiment's "no trap fired"
//      evidence, expected 0.
//   2. Write 1 to the CLINT msip register for hart 0 (32-bit access:
//      this QEMU's CLINT model only accepts 4-byte accesses to msip,
//      see the defect note in src/msip/PROOF.md, backlog item 70).
//      Read back msip (must be 1) and mip (MSIP, bit 3, must be 1).
//   3. Spin in a 100,000-mcycle polling window with the bit asserted,
//      reading no CSRs that could perturb mip, then read mip again:
//      MSIP must still be 1, and the trap counter must still be 0.
//   4. Write 0 to msip, read back (must be 0), read mip: MSIP must be
//      0 again, trap counter still 0.
//   5. Re-read mstatus and mie; both enables must still be clear,
//      proving nothing could have been delivered in between.
//
// Every check increments the checks counter; a failed check prints
// FAIL and increments the mismatches counter. The verdict-relevant
// values (boot baselines, every msip/mip readback, the trap count)
// feed a 64-bit FNV-1a digest printed as the last data line, so the
// three bench runs can be compared for byte-identical output.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.
//
// No labels-as-values anywhere, so the GCC label-miscompile concern
// does not arise.

#include "../uart.h"

#define CLINT_MSIP0   0x02000000UL  // CLINT msip for hart 0, 32-bit register
#define SPIN_CYCLES   100000UL

#define MSTATUS_MIE   (1UL << 3)
#define MIE_MSIE      (1UL << 3)
#define MIP_MSIP      (1UL << 3)    // mip bit 3: machine software interrupt pending

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

extern void mpnt_trap_entry(void);

// Trap scratch: slot 0 trap count, slot 1 mcause, slot 2 mepc,
// slot 3 mtval, slot 4 parked t1. BSS-cleared to zero by boot.S.
unsigned long mpnt_regs[5];

static volatile unsigned int *const msip0 = (volatile unsigned int *)CLINT_MSIP0;

static unsigned long read_mip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mip" : "=r"(v));
    return v;
}

static unsigned long read_mie(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mie" : "=r"(v));
    return v;
}

static unsigned long read_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static unsigned long read_mcycle(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mcycle" : "=r"(v));
    return v;
}

// FNV-1a 64-bit over the verdict-relevant values.
static unsigned long long fnv = 14695981039346656037ULL;
static void digest64(unsigned long v) {
    for (int i = 0; i < 8; i++) {
        fnv ^= (unsigned long long)((v >> (i * 8)) & 0xffUL);
        fnv *= 1099511628211ULL;
    }
}

static int checks = 0;
static int mismatches = 0;

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        mismatches++;
    }
}

int main(void) {
    unsigned long mie, mstatus, mip_boot, mtvec;
    unsigned long msip_rb, mip_set, mip_window, mip_clear, traps;
    unsigned long t0, t1;

    uart_init();
    uart_puts("mip-pending-no-trap: mip reflects a pended interrupt with MIE clear\n");

    // Boot baselines: both enable bits must read clear.
    mie = read_mie();
    mstatus = read_mstatus();
    mip_boot = read_mip();
    uart_puts("boot: mie.MSIE=");
    uart_put_dec((mie >> 3) & 1UL);
    uart_puts(" mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(" mip=");
    uart_put_hex(mip_boot);
    uart_puts("\n");
    digest64(mie & MIE_MSIE);
    digest64(mstatus & MSTATUS_MIE);
    digest64(mip_boot);
    check((mie & MIE_MSIE) == 0, "mie.MSIE set at boot");
    check((mstatus & MSTATUS_MIE) == 0, "mstatus.MIE set at boot");
    check((mip_boot & MIP_MSIP) == 0, "mip.MSIP set at boot");

    // Belt and suspenders: explicitly clear both enables and read
    // back, so the run never depends on the reset value.
    __asm__ volatile("csrc mie, %0" : : "r"(MIE_MSIE));
    __asm__ volatile("csrc mstatus, %0" : : "r"(MSTATUS_MIE));
    mie = read_mie();
    mstatus = read_mstatus();
    check((mie & MIE_MSIE) == 0, "mie.MSIE not clear after csrc");
    check((mstatus & MSTATUS_MIE) == 0, "mstatus.MIE not clear after csrc");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the recording area. Expected to never fire.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mpnt_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mpnt_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    uart_puts("trap: mtvec=");
    uart_put_hex(mtvec);
    uart_puts("\n");
    check((mtvec & ~3UL) == (unsigned long)mpnt_trap_entry,
          "mtvec did not take the handler address");
    check((mtvec & 3UL) == 0, "mtvec not in direct mode");

    // Set the pending bit: write 1 to msip, read back, read mip.
    *msip0 = 1;
    msip_rb = *msip0;
    mip_set = read_mip();
    uart_puts("set: msip-readback=");
    uart_put_dec(msip_rb);
    uart_puts(" mip=");
    uart_put_hex(mip_set);
    uart_puts(" (mip.MSIP=");
    uart_put_dec((mip_set & MIP_MSIP) ? 1UL : 0UL);
    uart_puts(")\n");
    digest64(msip_rb);
    digest64(mip_set);
    check(msip_rb == 1, "msip readback != 1 after set");
    check((mip_set & MIP_MSIP) == MIP_MSIP,
          "mip.MSIP not set after msip set, MIE clear");

    // The 100,000-cycle window: poll mcycle with the bit asserted.
    // No CSR reads that could perturb mip; the only cross-check at
    // the end is the trap counter.
    t0 = read_mcycle();
    do {
        t1 = read_mcycle();
    } while (t1 - t0 < SPIN_CYCLES);

    // Read mip with the bit still asserted; the pending bit must
    // have survived the whole window and no trap may have fired.
    mip_window = read_mip();
    traps = mpnt_regs[0];
    uart_puts("window: cycles=");
    uart_put_dec(t1 - t0);
    uart_puts(" mip=");
    uart_put_hex(mip_window);
    uart_puts(" traps=");
    uart_put_dec(traps);
    uart_puts("\n");
    digest64(mip_window);
    digest64(traps);
    check((t1 - t0) >= SPIN_CYCLES, "spin window shorter than 100000 cycles");
    check((mip_window & MIP_MSIP) == MIP_MSIP,
          "mip.MSIP not still set after the 100k-cycle window");
    check(traps == 0, "trap fired during the 100k-cycle window with MIE clear");

    // Clear the pending bit: write 0 to msip, read back, read mip.
    *msip0 = 0;
    msip_rb = *msip0;
    mip_clear = read_mip();
    uart_puts("clear: msip-readback=");
    uart_put_dec(msip_rb);
    uart_puts(" mip=");
    uart_put_hex(mip_clear);
    uart_puts(" (mip.MSIP=");
    uart_put_dec((mip_clear & MIP_MSIP) ? 1UL : 0UL);
    uart_puts(")\n");
    digest64(msip_rb);
    digest64(mip_clear);
    check(msip_rb == 0, "msip readback != 0 after clear");
    check((mip_clear & MIP_MSIP) == 0, "mip.MSIP still set after msip clear");

    // Both enables must still be clear at the end, and no trap may
    // have fired anywhere in the run.
    mie = read_mie();
    mstatus = read_mstatus();
    traps = mpnt_regs[0];
    uart_puts("end: mie.MSIE=");
    uart_put_dec((mie >> 3) & 1UL);
    uart_puts(" mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(" traps=");
    uart_put_dec(traps);
    uart_puts("\n");
    digest64(traps);
    check((mie & MIE_MSIE) == 0, "mie.MSIE changed during the run");
    check((mstatus & MSTATUS_MIE) == 0, "mstatus.MIE changed during the run");
    check(traps == 0, "trap fired during the run");

    uart_puts("checks: ");
    uart_put_dec((unsigned long)checks);
    uart_puts(" mismatches: ");
    uart_put_dec((unsigned long)mismatches);
    uart_puts("\n");
    uart_puts("digest: ");
    uart_put_hex(fnv);
    uart_puts("\n");

    if (mismatches == 0) {
        uart_puts("RESULT: PASS\n");
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)
            ;
    }
    uart_puts("RESULT: FAIL\n");
    // Park the hart; the harness observes the timeout exit status.
    for (;;) {
        __asm__ volatile("wfi");
    }
}

