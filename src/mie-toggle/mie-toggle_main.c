// mie-toggle_main.c: mstatus.MIE as the global interrupt-enable gate
// (backlog item 156).
//
// Exactly one mechanism is under test: a pending interrupt whose
// enable bit is set in mie does not reach the trap handler while
// mstatus.MIE is clear, and reaches it exactly once the moment MIE
// is set.
//
// Sequence under test:
//   1. Enable only the machine timer interrupt (mie.MTIE) with
//      mstatus.MIE clear. Arm the CLINT timer: mtimecmp =
//      mtime + ARM_AHEAD_TICKS.
//   2. Gated window: spin for GATED_WINDOW_TICKS mtime ticks with MIE
//      still clear. No trap may fire in that window; mip.MTIP must
//      read 1, proving the interrupt is pending but gated.
//   3. Set MIE (csrsi mstatus, 0x8) and wait. Exactly one trap must
//      arrive with mcause == 0x8000000000000007 (machine timer
//      interrupt, code 7). The handler disarms the timer
//      (mtimecmp = all-ones), so MTIP drops and the trap cannot
//      re-fire.
//   4. Quiet window: another QUIET_WINDOW_TICKS mtime ticks with MIE
//      set. The trap counter must stay at 1 and MTIP must stay 0.
//
// All wait windows are bounded by mtime ticks, never by
// instruction-spin counts alone, so a broken machine yields FAIL
// lines, not a hang. The verdict-relevant output lines carry only
// register values, no cycle counts or host-time numbers, so they are
// byte-identical across runs; a 64-bit FNV-1a checksum over the
// measured verdict values is printed and cross-checked in the
// PROOF.md results table.

#include "../uart.h"

// QEMU virt CLINT: mtime is a 64-bit register at offset 0xbff8,
// mtimecmp for hart 0 is a 64-bit register at offset 0x4000.
#define CLINT_MTIME     0x0200bff8UL
#define CLINT_MTIMECMP0 0x02004000UL

#define MSTATUS_MIE (1UL << 3)
#define MIE_MTIE    (1UL << 7)
#define MIP_MTIP    (1UL << 7)

// Interrupt bit plus exception code 7 = machine timer interrupt.
#define MCAUSE_MTI 0x8000000000000007UL

#define ARM_AHEAD_TICKS    5000UL    // timer fires 500 us of virtual time out
#define GATED_WINDOW_TICKS 100000UL // 10 ms: pending-but-gated observation
#define QUIET_WINDOW_TICKS 100000UL // 10 ms: no re-fire observation

static volatile unsigned long mie_regs[8]; // trap scratch, mscratch points here
static volatile unsigned long trap_count = 0;
static volatile unsigned long last_mcause = 0;
static volatile unsigned long last_mepc = 0;

static volatile unsigned long *const mtime =
    (volatile unsigned long *)CLINT_MTIME;
static volatile unsigned long *const mtimecmp0 =
    (volatile unsigned long *)CLINT_MTIMECMP0;

static unsigned long csr_read_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

static unsigned long checks = 0;
static unsigned long fails = 0;

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// Called from the asm trap entry. Runs with interrupts still enabled
// state intact; disarms the timer first (level-triggered source: the
// trap would re-fire the instant mret restores MIE if mtimecmp were
// still armed), then counts the trap and records what it saw.
void mie_c_handle(void) {
    *mtimecmp0 = ~0UL;
    trap_count++;
    last_mcause = mie_regs[2];
    last_mepc = mie_regs[3];
}

extern void mie_toggle_trap_entry(void);

// Spin for a bounded number of mtime ticks. mtime always advances on
// this QEMU, so the window always terminates; interrupts cannot fire
// here because MIE is clear (gated window) or the source is disarmed
// (quiet window).
static void spin_ticks(unsigned long ticks) {
    unsigned long t0 = *mtime;
    while (*mtime - t0 < ticks)
        ;
}

// Wait for the trap with a large spin budget so a broken delivery is
// a FAIL, not a hang.
static void wait_for_trap(unsigned long budget) {
    while (trap_count < 1 && budget--)
        ;
}

static unsigned long fnv1a64(const unsigned char *data, unsigned long len) {
    unsigned long h = 1469598103934665603UL;
    unsigned long i;
    for (i = 0; i < len; i++) {
        h ^= (unsigned long)data[i];
        h *= 1099511628211UL;
    }
    return h;
}

int main(void) {
    unsigned long tv, mie, mstatus, mip, cmp, mcause;
    unsigned long mtip_gated, traps_gated, traps_after, mtimecmp_after;
    unsigned long mtip_after, traps_quiet, mtip_quiet, mie_after;
    unsigned long words[10], checksum;

    uart_init();
    uart_puts("mie-toggle: mstatus.MIE global interrupt-enable gate test\n");
    uart_puts("hart mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    // Install the trap handler: direct-mode mtvec, mscratch pointing at
    // the scratch area. Enable only the machine timer interrupt.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mie_toggle_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mie_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
    uart_puts("trap: mtvec=");
    uart_put_hex(tv);
    uart_puts("\n");
    check((tv & ~3UL) == (unsigned long)mie_toggle_trap_entry,
          "mtvec did not take the handler address");
    check((tv & 3UL) == 0, "mtvec not in direct mode");
    __asm__ volatile("csrs mie, %0" : : "r"(MIE_MTIE));
    // The gate under test starts CLEAR: pending interrupts must not
    // reach the handler.
    __asm__ volatile("csrc mstatus, %0" : : "r"(MSTATUS_MIE));
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    __asm__ volatile("csrr %0, mstatus" : "=r"(mstatus));
    uart_puts("irq-enable: mie.MTIE=");
    uart_put_dec((mie >> 7) & 1UL);
    uart_puts(" mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts("\n");
    check(((mie >> 7) & 1UL) == 1, "mie.MTIE not set");
    check(((mstatus >> 3) & 1UL) == 0, "mstatus.MIE not clear at arm time");

    // Arm the timer 5000 ticks out. The programmed value depends on
    // the boot-time mtime, so the printed line reports only the
    // match outcome; the check itself compares the real readback.
    cmp = *mtime + ARM_AHEAD_TICKS;
    *mtimecmp0 = cmp;
    check(*mtimecmp0 == cmp, "mtimecmp readback != programmed value");
    uart_puts("arm: mtimecmp-readback-match=");
    uart_put_dec(*mtimecmp0 == cmp ? 1UL : 0UL);
    uart_puts(" (expect 1)\n");

    // Gated window: MIE clear, so even though mtime runs past
    // mtimecmp and MTIP goes pending, no trap may fire.
    spin_ticks(GATED_WINDOW_TICKS);
    __asm__ volatile("csrr %0, mip" : "=r"(mip));
    traps_gated = trap_count;
    mtip_gated = (mip >> 7) & 1UL;
    uart_puts("gated: traps-during-window=");
    uart_put_dec(traps_gated);
    uart_puts(" mip.MTIP=");
    uart_put_dec(mtip_gated);
    uart_puts(" (expect 0 / 1)\n");
    check(traps_gated == 0, "trap fired with mstatus.MIE clear");
    check(mtip_gated == 1, "mip.MTIP not pending during the gated window");

    // Open the gate.
    __asm__ volatile("csrsi mstatus, 8");
    __asm__ volatile("csrr %0, mstatus" : "=r"(mstatus));
    uart_puts("release: mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts("\n");
    check(((mstatus >> 3) & 1UL) == 1, "mstatus.MIE not set after csrsi");
    mie_after = (mstatus >> 3) & 1UL;

    wait_for_trap(10000000UL);
    __asm__ volatile("csrr %0, mip" : "=r"(mip));
    traps_after = trap_count;
    mcause = last_mcause;
    mtimecmp_after = *mtimecmp0;
    mtip_after = (mip >> 7) & 1UL;
    uart_puts("trap1: mcause=");
    uart_put_hex(mcause);
    uart_puts(" traps=");
    uart_put_dec(traps_after);
    uart_puts("\n");
    check(traps_after == 1, "trap never fired after MIE set");
    check(mcause == MCAUSE_MTI, "trap mcause != 0x8000000000000007");
    uart_puts("disarm: mtimecmp=");
    uart_put_hex(mtimecmp_after);
    uart_puts(" mip.MTIP=");
    uart_put_dec(mtip_after);
    uart_puts(" (expect 0xffffffffffffffff / 0)\n");
    check(mtimecmp_after == ~0UL, "handler did not disarm mtimecmp");
    check(mtip_after == 0, "mip.MTIP still pending after disarm");

    // Quiet window: the gate is open and the source is disarmed, so
    // nothing may fire. The counter must stay at 1.
    spin_ticks(QUIET_WINDOW_TICKS);
    __asm__ volatile("csrr %0, mip" : "=r"(mip));
    traps_quiet = trap_count;
    mtip_quiet = (mip >> 7) & 1UL;
    uart_puts("quiet: traps=");
    uart_put_dec(traps_quiet);
    uart_puts(" mip.MTIP=");
    uart_put_dec(mtip_quiet);
    uart_puts(" (expect 1 / 0)\n");
    check(traps_quiet == 1, "re-delivery fired during the quiet window");
    check(mtip_quiet == 0, "mip.MTIP pending during the quiet window");

    // Nothing else may fire; stop the gate before parking.
    __asm__ volatile("csrc mie, %0" : : "r"(MIE_MTIE));
    __asm__ volatile("csrc mstatus, %0" : : "r"(MSTATUS_MIE));

    words[0] = mtip_gated;
    words[1] = traps_gated;
    words[2] = mcause;
    words[3] = traps_after;
    words[4] = mtimecmp_after;
    words[5] = mtip_after;
    words[6] = traps_quiet;
    words[7] = mtip_quiet;
    words[8] = mie_after;
    words[9] = ((mie >> 7) & 1UL);
    checksum = fnv1a64((const unsigned char *)words, sizeof(words));

    uart_puts("checks=");
    uart_put_dec(checks);
    uart_puts(" mismatches=");
    uart_put_dec(fails);
    uart_puts("\n");
    uart_puts("checksum=");
    uart_put_hex(checksum);
    uart_puts("\n");
    if (fails == 0)
        uart_puts("RESULT: PASS\n");
    else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec(fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
