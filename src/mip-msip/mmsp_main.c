// mmsp_main.c: CLINT msip-to-mip pending-bit tracking check
// (backlog item 111).
//
// Mechanism under test: whether writing the CLINT msip register for
// hart 0 drives the MSIP pending bit (bit 3) of the mip CSR, with no
// trap enabled. The machine software interrupt stays disabled for
// the whole run (mie.MSIE = 0, mstatus.MIE = 0, both read back), so
// no trap can fire; the module observes only the pending bit
// transitioning as the msip register is written and cleared.
//
// Sequence under test, two set/clear cycles:
//   1. Read mie and mstatus; require mie.MSIE = 0 and mstatus.MIE = 0.
//   2. Read mip and msip; require both 0 (bit 3 of mip clear).
//   3. Write 1 to msip (32-bit: this QEMU's CLINT model only accepts
//      4-byte accesses to msip, see backlog item 70), read it back
//      (must be 1), read mip (bit 3 must be 1, all other bits still 0).
//   4. Write 0 to msip, read back (must be 0), read mip (bit 3 must
//      be 0 again).
//   5. Repeat steps 3-4 once, then re-read mie and mstatus and
//      require them unchanged (still clear).
//
// A failed check prints FAIL and flips the verdict; RESULT: PASS is
// printed only when every check held.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.
//
// No trap handler: with the interrupt never enabled, nothing can be
// taken, so the image has no trap vector beyond boot.S's default.

#include "../uart.h"

// QEMU virt CLINT base; msip for hart 0 is a 32-bit register at the base.
#define CLINT_MSIP0   0x02000000UL
#define CLINT_MTIME   0x0200bff8UL  // 64-bit mtime, UART-drain timebase
#define CLINT_MTIMECMP0 0x02004000UL // 64-bit mtimecmp for hart 0

#define MSTATUS_MIE   (1UL << 3)
#define MIE_MSIE      (1UL << 3)
#define MIP_MSIP      (1UL << 3)    // mip bit 3: machine software interrupt pending

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

static volatile unsigned int *const msip0 = (volatile unsigned int *)CLINT_MSIP0;

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

static unsigned long read_mip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mip" : "=r"(v));
    return v;
}

static unsigned long read_mtime(void) {
    return *(volatile unsigned long *)CLINT_MTIME;
}

static unsigned long read_mtimecmp0(void) {
    return *(volatile unsigned long *)CLINT_MTIMECMP0;
}

// The pending bit of interest is mip bit 3 (MSIP). The rest of mip
// may carry unrelated pending bits; on this machine bit 7 (MTIP) is
// set at boot because the CLINT mtimecmp registers reset to 0 below
// mtime. The module tracks bit 3's transitions and requires every
// other mip bit to stay byte-identical across the run, so an
// unrelated pending bit cannot leak into the verdict.
static unsigned long boot_other_bits;

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// One set/clear cycle: drive msip from 0 to 1 to 0 and verify the
// mip.MSIP bit follows each transition. No MIE toggling here; the
// whole run keeps interrupts disabled.
static void cycle(int n) {
    unsigned long msip_rb, mip_rb;

    // Set: write 1, read back, mip bit 3 must be set and nothing
    // else in mip may have changed.
    *msip0 = 1;
    msip_rb = *msip0;
    mip_rb = read_mip();
    uart_puts("set");
    uart_put_dec((unsigned long)n);
    uart_puts(": msip-readback=");
    uart_put_dec(msip_rb);
    uart_puts(" mip=");
    uart_put_hex(mip_rb);
    uart_puts(" (mip.MSIP=");
    uart_put_dec((mip_rb & MIP_MSIP) ? 1UL : 0UL);
    uart_puts(")\n");
    check(msip_rb == 1, "msip readback != 1 after set");
    check((mip_rb & MIP_MSIP) == MIP_MSIP, "mip.MSIP not set after msip set");
    check((mip_rb & ~MIP_MSIP) == boot_other_bits,
          "non-MSIP mip bits changed across the set transition");

    // Clear: write 0, read back, mip bit 3 must be clear again.
    *msip0 = 0;
    msip_rb = *msip0;
    mip_rb = read_mip();
    uart_puts("clear");
    uart_put_dec((unsigned long)n);
    uart_puts(": msip-readback=");
    uart_put_dec(msip_rb);
    uart_puts(" mip=");
    uart_put_hex(mip_rb);
    uart_puts(" (mip.MSIP=");
    uart_put_dec((mip_rb & MIP_MSIP) ? 1UL : 0UL);
    uart_puts(")\n");
    check(msip_rb == 0, "msip readback != 0 after clear");
    check((mip_rb & MIP_MSIP) == 0, "mip.MSIP still set after msip clear");
    check((mip_rb & ~MIP_MSIP) == boot_other_bits,
          "non-MSIP mip bits changed across the clear transition");
}

int main(void) {
    unsigned long mie, mstatus, mip, msip0v, mtime, mtimecmp;

    uart_init();
    uart_puts("mip-msip: CLINT msip-to-mip pending-bit tracking check\n");

    // 1+2. Boot state: the interrupt must be disabled and both the
    // register and the pending bit must start clear.
    mie = read_mie();
    mstatus = read_mstatus();
    mip = read_mip();
    msip0v = *msip0;
    uart_puts("boot: mie.MSIE=");
    uart_put_dec((mie >> 3) & 1UL);
    uart_puts(" mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(" mip=");
    uart_put_hex(mip);
    uart_puts(" msip=");
    uart_put_dec(msip0v);
    uart_puts("\n");
    check(((mie >> 3) & 1UL) == 0, "mie.MSIE set at boot");
    check(((mstatus >> 3) & 1UL) == 0, "mstatus.MIE set at boot");
    check((mip & MIP_MSIP) == 0, "mip.MSIP set at boot");
    check(msip0v == 0, "msip nonzero at boot");
    boot_other_bits = mip & ~MIP_MSIP;
    mtime = read_mtime();
    mtimecmp = read_mtimecmp0();
    uart_puts("note: non-MSIP mip bits at boot=");
    uart_put_hex(boot_other_bits);
    uart_puts(" mtime=");
    uart_put_hex(mtime);
    uart_puts(" mtimecmp0=");
    uart_put_hex(mtimecmp);
    uart_puts("\n");
    check(((mip >> 7) & 1UL) == ((mtime >= mtimecmp) ? 1UL : 0UL),
          "mip bit 7 does not track mtime >= mtimecmp");

    // 3-5. Two set/clear cycles, then confirm the interrupt is
    // still disabled (nothing could have been taken in between).
    cycle(1);
    cycle(2);

    mie = read_mie();
    mstatus = read_mstatus();
    uart_puts("end: mie.MSIE=");
    uart_put_dec((mie >> 3) & 1UL);
    uart_puts(" mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts("\n");
    check(((mie >> 3) & 1UL) == 0, "mie.MSIE changed during the run");
    check(((mstatus >> 3) & 1UL) == 0, "mstatus.MIE changed during the run");

    uart_puts("RESULT: ");
    uart_puts(fails == 0 ? "PASS" : "FAIL");
    uart_puts("\n");

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
