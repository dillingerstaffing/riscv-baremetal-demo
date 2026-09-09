// msip_main.c: CLINT software-interrupt (msip) delivery check
// (backlog item 70).
//
// Exactly one mechanism is under test: a write of 1 to the CLINT msip
// register for hart 0 must deliver a machine software interrupt, and
// clearing msip must end delivery with no re-fire.
//
// Sequence under test:
//   1. Control: with mie.MSIE and mstatus.MIE set but msip clear, poll
//      briefly; no trap may arrive (proves traps are not spurious).
//   2. Write 1 to msip, read it back, and wait for the trap. The trap
//      handler records mcause/mepc/mtval and an entry rdcycle stamp,
//      then clears msip and bumps the trap counter.
//   3. Check mcause == 0x8000000000000003 (machine software
//      interrupt, code 3) and that msip reads 0 after the handler
//      cleared it. Poll again: the counter must stay at 1, i.e. no
//      re-delivery.
//   4. Second set/clear cycle: set msip again, wait for trap 2, verify
//      mcause again, confirm msip reads 0 and the counter stays at 2.
//
// The only interrupt source enabled is the machine software interrupt,
// so the mcause value and the single-trap count are the ground truth
// for the delivery path. Delivery latency is stamped in rdcycle units
// and calibrated against the CLINT mtime.

#include "../uart.h"

// QEMU virt CLINT base; msip for hart 0 is a 32-bit register at the base.
#define CLINT_MSIP0   0x02000000UL
#define CLINT_MTIME   0x0200bff8UL  // 64-bit mtime, 10 MHz timebase

#define MSTATUS_MIE   (1UL << 3)
#define MIE_MSIE      (1UL << 3)

// Interrupt bit plus exception code 3 = machine software interrupt.
#define MCAUSE_MSI    0x8000000000000003UL

static volatile unsigned long msip_regs[8];  // trap scratch, mscratch points here
static volatile unsigned long trap_count = 0;
static volatile unsigned long last_mcause = 0;
static volatile unsigned long last_mepc = 0;
static volatile unsigned long last_entry_cycle = 0;

static volatile unsigned int *const msip0 = (volatile unsigned int *)CLINT_MSIP0;

static unsigned long rdcycle(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, cycle" : "=r"(v));
    return v;
}

static unsigned long csr_read_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

static unsigned long clint_get_mtime(void) {
    return *(volatile unsigned long *)CLINT_MTIME;
}

// rdcycle on this QEMU follows the host clock, not the 10 MHz mtime
// timebase. Spin for 1M mtime ticks (100 ms of virtual time) and report
// rdcycle units per tick so the delivery-latency numbers are
// interpretable as host time. (Same construction as src/plic/.)
static unsigned long calibration(void) {
    unsigned long t0, t1, c0, c1, ratio;

    t0 = clint_get_mtime();
    c0 = rdcycle();
    while (clint_get_mtime() - t0 < 1000000UL)
        ;
    t1 = clint_get_mtime();
    c1 = rdcycle();
    ratio = (c1 - c0) / (t1 - t0);
    uart_puts("clock: 1000000 mtime ticks -> rdcycle delta ");
    uart_put_dec(c1 - c0);
    uart_puts(" (ratio ");
    uart_put_dec(ratio);
    uart_puts(" rdcycle units per tick)\n");
    return ratio;
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

// Called from the asm trap entry. Runs with interrupts pending state
// unchanged; clears msip first (level-triggered source: the trap would
// re-fire the instant mret restores MIE if msip were still set), then
// records what the handler observed.
void msip_c_handle(void) {
    *msip0 = 0;
    trap_count++;
    last_mcause = msip_regs[2];
    last_mepc = msip_regs[3];
    last_entry_cycle = msip_regs[5];
}

extern void msip_trap_entry(void);

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

int main(void) {
    unsigned long ratio, spins, set_cycle;
    unsigned long latency1, latency2;
    unsigned long mcause1, mcause2, mepc1, mepc2;

    uart_init();
    uart_puts("msip-delivery: CLINT software-interrupt delivery test\n");
    uart_puts("hart mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    ratio = calibration();
    check(ratio >= 145 && ratio <= 155,
          "rdcycle/mtime ratio outside 145..155");

    // Install the trap handler: direct-mode mtvec, mscratch pointing at
    // the scratch area. Enable the machine software interrupt and the
    // global MIE bit; read both back.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)msip_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)msip_regs));
    {
        unsigned long tv;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        uart_puts("trap: mtvec=");
        uart_put_hex(tv);
        uart_puts("\n");
        check((tv & ~3UL) == (unsigned long)msip_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
    }
    __asm__ volatile("csrs mie, %0" : : "r"(MIE_MSIE));
    __asm__ volatile("csrs mstatus, %0" : : "r"(MSTATUS_MIE));
    {
        unsigned long mie, mstatus;
        __asm__ volatile("csrr %0, mie" : "=r"(mie));
        __asm__ volatile("csrr %0, mstatus" : "=r"(mstatus));
        uart_puts("irq-enable: mie.MSIE=");
        uart_put_dec((mie >> 3) & 1UL);
        uart_puts(" mstatus.MIE=");
        uart_put_dec((mstatus >> 3) & 1UL);
        uart_puts("\n");
        check(((mie >> 3) & 1UL) == 1, "mie.MSIE not set");
        check(((mstatus >> 3) & 1UL) == 1, "mstatus.MIE not set");
    }

    // Control: msip reads 0 before any set, and no trap arrives while
    // the interrupt is enabled but unasserted.
    uart_puts("control: msip-before-set=");
    uart_put_dec(*msip0);
    uart_puts("\n");
    check(*msip0 == 0, "msip nonzero before the first set");
    (void)quiet_poll(1000000UL);
    uart_puts("control: traps-with-msip-clear=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 0)\n");
    check(trap_count == 0, "spurious trap with msip clear");

    // Cycle 1: set msip, read back, wait for exactly one trap. Delivery
    // is paused (mstatus.MIE cleared) around the set+readback so the
    // trap handler cannot clear msip between the store and the load;
    // the interrupt is then re-enabled and must fire.
    __asm__ volatile("csrc mstatus, %0" : : "r"(MSTATUS_MIE));
    *msip0 = 1;
    {
        unsigned long rb = *msip0;
        set_cycle = rdcycle();   // stamped before MIE is re-enabled, so the
                                 // handler entry stamp is always later
        __asm__ volatile("csrs mstatus, %0" : : "r"(MSTATUS_MIE));
        uart_puts("set1: msip-readback=");
        uart_put_dec(rb);
        uart_puts("\n");
        check(rb == 1, "msip readback != 1 after set");
    }
    spins = wait_for_trap(1, 10000000UL);
    uart_puts("trap1: spins=");
    uart_put_dec(spins);
    uart_puts("\n");
    check(spins < 10000000UL, "machine software interrupt never delivered");
    mcause1 = last_mcause;
    mepc1 = last_mepc;
    latency1 = last_entry_cycle - set_cycle;
    uart_puts("trap1: mcause=");
    uart_put_hex(mcause1);
    uart_puts(" mepc=");
    uart_put_hex(mepc1);
    uart_puts(" delivery-cycles=");
    uart_put_dec(latency1);
    uart_puts("\n");
    check(mcause1 == MCAUSE_MSI, "trap1 mcause != 0x8000000000000003");
    check(trap_count == 1, "trap_count != 1 after first set");

    // The handler cleared msip: read 0, and the counter must not move
    // during a further quiet window (no re-delivery).
    uart_puts("clear1: msip-after-handler=");
    uart_put_dec(*msip0);
    uart_puts("\n");
    check(*msip0 == 0, "msip not cleared by the trap handler");
    (void)quiet_poll(2000000UL);
    uart_puts("clear1: traps-after-quiet=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 1)\n");
    check(trap_count == 1, "re-delivery fired after msip cleared");

    // Cycle 2: same set/clear again, proving the path re-arms. Same
    // MIE pause around set+readback as cycle 1.
    __asm__ volatile("csrc mstatus, %0" : : "r"(MSTATUS_MIE));
    *msip0 = 1;
    {
        unsigned long rb2 = *msip0;
        set_cycle = rdcycle();   // before MIE re-enable, as in cycle 1
        __asm__ volatile("csrs mstatus, %0" : : "r"(MSTATUS_MIE));
        uart_puts("set2: msip-readback=");
        uart_put_dec(rb2);
        uart_puts("\n");
        check(rb2 == 1, "msip readback != 1 on second set");
    }
    spins = wait_for_trap(2, 10000000UL);
    uart_puts("trap2: spins=");
    uart_put_dec(spins);
    uart_puts("\n");
    check(spins < 10000000UL, "second software interrupt never delivered");
    mcause2 = last_mcause;
    mepc2 = last_mepc;
    latency2 = last_entry_cycle - set_cycle;
    uart_puts("trap2: mcause=");
    uart_put_hex(mcause2);
    uart_puts(" mepc=");
    uart_put_hex(mepc2);
    uart_puts(" delivery-cycles=");
    uart_put_dec(latency2);
    uart_puts("\n");
    check(mcause2 == MCAUSE_MSI, "trap2 mcause != 0x8000000000000003");
    check(trap_count == 2, "trap_count != 2 after second set");
    uart_puts("clear2: msip-after-handler=");
    uart_put_dec(*msip0);
    uart_puts("\n");
    check(*msip0 == 0, "msip not cleared by the second trap handler");
    (void)quiet_poll(2000000UL);
    uart_puts("clear2: traps-after-quiet=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 2)\n");
    check(trap_count == 2, "re-delivery fired after second clear");

    // Disable the interrupt before finishing: nothing else may fire.
    __asm__ volatile("csrc mie, %0" : : "r"(MIE_MSIE));

    if (fails == 0) {
        uart_puts("RESULT: PASS (mcause=0x8000000000000003 x2, no re-delivery)\n");
    } else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
