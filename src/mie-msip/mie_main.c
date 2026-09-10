// mie_main.c: mie.MSIE enable-bit gate check (backlog item 112).
//
// Mechanism under test: the MSIE bit (bit 3) of the mie CSR gates
// delivery of the machine software interrupt raised by the CLINT
// msip register, independent of the global mstatus.MIE bit, which
// stays set for the whole run. Clearing MSIE in mie stops delivery
// even with msip asserted and the global bit on; setting MSIE lets
// the pending interrupt trap exactly once, and clearing MSIE again
// stops a freshly asserted msip from trapping.
//
// Sequence under test:
//   1. Boot: mie reads 0, mstatus.MIE reads 0. Install the trap
//      handler (direct-mode mtvec, mscratch scratch area), set
//      mstatus.MIE, and read both back. mstatus.MIE stays set for
//      the rest of the run, so mie.MSIE is the only gating variable.
//   2. Phase 1 (MSIE clear): write mie = 0 with csrw, read back the
//      full word, require mie == 0. Set msip = 1 (readback 1), spin
//      a bounded quiet window, require trap_count == 0. Clear msip.
//   3. Phase 2 (MSIE set): set bit 3 with csrsi, read back mie,
//      require mie == 0x8. Clear then re-set msip: the set+readback
//      is done with mstatus.MIE paused so the handler cannot clear
//      msip between the store and the load, then MIE is restored and
//      the trap must fire. Wait for trap_count == 1 and require
//      mcause == 0x8000000000000003 (machine software interrupt,
//      code 3). A further quiet window must leave the count at 1,
//      i.e. no re-delivery after the handler cleared msip.
//   4. Re-gate: clear bit 3 with csrci, read back mie, require
//      mie == 0. Set msip = 1 again (readback 1), quiet window,
//      require trap_count still 1: the mie write alone stops
//      delivery. Clear msip.
//
// The machine software interrupt is the only enabled interrupt
// source (mie.MTIE and mie.MEIE are never set), so the mcause value
// and the trap counts are the ground truth for the gate. The trap
// handler clears msip on entry (level-triggered source: the trap
// would re-fire the instant mret runs if msip were still set),
// records mcause/mepc/mtval, and bumps the trap counter.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.

#include "../uart.h"

// QEMU virt CLINT base; msip for hart 0 is a 32-bit register at the base.
#define CLINT_MSIP0   0x02000000UL
#define CLINT_MTIME   0x0200bff8UL  // 64-bit mtime, UART-drain timebase

#define MSTATUS_MIE   (1UL << 3)
#define MIE_MSIE      (1UL << 3)    // mie bit 3: machine software interrupt enable

// Interrupt bit plus exception code 3 = machine software interrupt.
#define MCAUSE_MSI    0x8000000000000003UL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

static volatile unsigned long mie_regs[8];  // trap scratch, mscratch points here
static volatile unsigned long trap_count = 0;
static volatile unsigned long last_mcause = 0;
static volatile unsigned long last_mepc = 0;

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

static unsigned long read_mtime(void) {
    return *(volatile unsigned long *)CLINT_MTIME;
}

// Called from the asm trap entry. Clears msip first (level-triggered
// source), then records what the handler observed.
void mie_c_handle(void) {
    *msip0 = 0;
    trap_count++;
    last_mcause = mie_regs[2];
    last_mepc = mie_regs[3];
}

extern void mie_trap_entry(void);

// Poll with a spin budget until the counter reaches want, so a broken
// delivery is a FAIL, not a hang.
static unsigned long wait_for_trap(unsigned long want, unsigned long budget) {
    unsigned long spins = 0;
    while (trap_count < want && spins < budget)
        spins++;
    return spins;
}

static unsigned long quiet_poll(unsigned long budget) {
    unsigned long spins = 0;
    while (spins < budget)
        spins++;
    return spins;
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
    unsigned long mie, mstatus, spins;

    uart_init();
    uart_puts("mie-msip: mie.MSIE enable-bit gate check\n");

    // 1. Boot state: both enable bits clear before anything runs.
    mie = read_mie();
    mstatus = read_mstatus();
    uart_puts("boot: mie=");
    uart_put_hex(mie);
    uart_puts(" mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts("\n");
    check(mie == 0, "mie nonzero at boot");
    check(((mstatus >> 3) & 1UL) == 0, "mstatus.MIE set at boot");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the scratch area. Then set the global MIE bit; it stays set
    // for the whole run so mie.MSIE is the only gating variable.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mie_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mie_regs));
    {
        unsigned long tv;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        uart_puts("trap: mtvec=");
        uart_put_hex(tv);
        uart_puts("\n");
        check((tv & ~3UL) == (unsigned long)mie_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
    }
    __asm__ volatile("csrs mstatus, %0" : : "r"(MSTATUS_MIE));
    mstatus = read_mstatus();
    uart_puts("irq-global: mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(" (stays set for the run)\n");
    check(((mstatus >> 3) & 1UL) == 1, "mstatus.MIE not set");

    // 2. Phase 1: MSIE clear. Write the full mie word to 0, read it
    // back, and require the write took.
    __asm__ volatile("csrw mie, zero");
    mie = read_mie();
    uart_puts("phase1: mie-after-clear=");
    uart_put_hex(mie);
    uart_puts(" (mie.MSIE=");
    uart_put_dec((mie >> 3) & 1UL);
    uart_puts(")\n");
    check(mie == 0, "mie readback != 0 after csrw mie, zero");

    // Assert msip with the interrupt masked at the mie level. No
    // trap may fire: the pending bit sits in mip but delivery is
    // gated off.
    *msip0 = 1;
    uart_puts("phase1: msip-readback=");
    uart_put_dec(*msip0);
    uart_puts("\n");
    check(*msip0 == 1, "msip readback != 1 after set");
    (void)quiet_poll(2000000UL);
    uart_puts("phase1: traps-with-MSIE-clear=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 0)\n");
    check(trap_count == 0, "trap fired while mie.MSIE was clear");
    *msip0 = 0;
    uart_puts("phase1: msip-after-clear=");
    uart_put_dec(*msip0);
    uart_puts("\n");
    check(*msip0 == 0, "msip readback != 0 after clear");

    // 3. Phase 2: MSIE set. Set bit 3, read back the full word, and
    // require mie == 0x8: only bit 3 moved.
    __asm__ volatile("csrsi mie, 8");
    mie = read_mie();
    uart_puts("phase2: mie-after-set=");
    uart_put_hex(mie);
    uart_puts(" (mie.MSIE=");
    uart_put_dec((mie >> 3) & 1UL);
    uart_puts(")\n");
    check(mie == MIE_MSIE, "mie readback != 0x8 after csrsi mie, 8");

    // Clear then re-set msip so the phase-2 trigger is deliberate.
    // Delivery is paused (mstatus.MIE cleared) around the set+readback
    // so the trap handler cannot clear msip between the store and the
    // load; the interrupt is then re-enabled and must fire.
    *msip0 = 0;
    __asm__ volatile("csrc mstatus, %0" : : "r"(MSTATUS_MIE));
    *msip0 = 1;
    {
        unsigned long rb = *msip0;
        __asm__ volatile("csrs mstatus, %0" : : "r"(MSTATUS_MIE));
        uart_puts("phase2: msip-readback=");
        uart_put_dec(rb);
        uart_puts("\n");
        check(rb == 1, "msip readback != 1 on phase-2 set");
    }
    spins = wait_for_trap(1, 10000000UL);
    uart_puts("phase2: spins-to-trap=");
    uart_put_dec(spins);
    uart_puts("\n");
    check(spins < 10000000UL, "no trap delivered with mie.MSIE set");
    uart_puts("phase2: mcause=");
    uart_put_hex(last_mcause);
    uart_puts(" mepc=");
    uart_put_hex(last_mepc);
    uart_puts(" trap-count=");
    uart_put_dec(trap_count);
    uart_puts("\n");
    check(last_mcause == MCAUSE_MSI, "trap mcause != 0x8000000000000003");
    check(trap_count == 1, "trap_count != 1 after phase-2 set");

    // The handler cleared msip: read 0, and the counter must not move
    // during a further quiet window (no re-delivery).
    uart_puts("phase2: msip-after-handler=");
    uart_put_dec(*msip0);
    uart_puts("\n");
    check(*msip0 == 0, "msip not cleared by the trap handler");
    (void)quiet_poll(2000000UL);
    uart_puts("phase2: traps-after-quiet=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 1)\n");
    check(trap_count == 1, "re-delivery fired after msip cleared");

    // 4. Re-gate: clear MSIE again, read back, and prove a freshly
    // asserted msip no longer traps. The mie write alone stops
    // delivery.
    __asm__ volatile("csrci mie, 8");
    mie = read_mie();
    uart_puts("regate: mie-after-clear=");
    uart_put_hex(mie);
    uart_puts(" (mie.MSIE=");
    uart_put_dec((mie >> 3) & 1UL);
    uart_puts(")\n");
    check(mie == 0, "mie readback != 0 after csrci mie, 8");
    *msip0 = 1;
    uart_puts("regate: msip-readback=");
    uart_put_dec(*msip0);
    uart_puts("\n");
    check(*msip0 == 1, "msip readback != 1 on re-gate set");
    (void)quiet_poll(2000000UL);
    uart_puts("regate: traps-with-MSIE-clear=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 1)\n");
    check(trap_count == 1, "trap fired after mie.MSIE was cleared again");
    *msip0 = 0;
    check(*msip0 == 0, "msip readback != 0 after final clear");

    // The global bit must be untouched: mie.MSIE was the only gating
    // variable for the whole run.
    mstatus = read_mstatus();
    uart_puts("end: mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts("\n");
    check(((mstatus >> 3) & 1UL) == 1, "mstatus.MIE changed during the run");

    uart_puts("RESULT: ");
    uart_puts(fails == 0 ? "PASS" : "FAIL");
    uart_puts(" (traps=");
    uart_put_dec(trap_count);
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
