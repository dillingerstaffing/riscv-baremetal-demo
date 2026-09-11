// mtie_main.c: mie.MTIE machine timer-interrupt enable-gate check
// (backlog item "riscv mie-mtie-gate").
//
// Mechanism under test: mie.MTIE (bit 7) is the M-mode enable bit
// for the machine timer interrupt, independent of the pending bit.
// With mstatus.MIE set and the CLINT timer armed so mip.MTIP goes
// pending while MTIE is clear, the pending bit must sit pending
// with zero traps; setting MTIE must then deliver exactly one
// trap. This is the enable-bit complement to the shipped
// src/mip-pending-no-trap (which measured the pending reflection
// with the enable off) and the sibling src/mie-msie-gate (the
// analogous gate for the software interrupt).
//
// Sequence under test:
//   1. Boot: mie reads 0, mstatus.MIE reads 0, mip.MTIP reads 0.
//      Disarm the timer (mtimecmp = all-ones) before anything
//      runs, install the trap handler (direct-mode mtvec,
//      mscratch scratch area), set mstatus.MIE, and read
//      everything back. MIE stays set for the whole run.
//   2. Phase A (MTIE clear): write mie = 0x0 with csrw, read it
//      back. Arm the CLINT timer mtimecmp = mtime + 5000 ticks
//      and poll until mip.MTIP (bit 7) reads pending, then spin a
//      bounded window (2,000,000 rdcycle deltas) and require
//      trap_count == 0 and mip.MTIP still pending, i.e. the
//      interrupt sat pending and globally enabled yet never
//      delivered.
//   3. Phase B (control): set MTIE with csrs mie, read back
//      mie == 0x80. The still-pending MTIP must now trap exactly
//      once: wait for trap_count == 1 and require
//      mcause == 0x8000000000000007 (machine timer interrupt,
//      code 7). The handler disarms mtimecmp to all-ones inside
//      the trap (writing the compare above the running mtime
//      clears MTIP and stops the re-fire), records mcause/mepc,
//      and bumps the counter.
//   4. Phase C (quiet window): another bounded rdcycle window must
//      leave the count at 1, i.e. no re-delivery after the
//      disarm.
//
// The CLINT msip is never pended and no S-mode source is ever
// programmed, so the machine timer interrupt is the only source
// that can ever be taken; the mcause value plus the trap counts
// are the ground truth for the gate.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.

#include "../uart.h"
#include "../preempt/clint.h"

#define MSTATUS_MIE   (1UL << 3)
#define MIE_MTIE      (1UL << 7)    // mie bit 7: machine timer interrupt enable
#define MIP_MTIP      (1UL << 7)    // mip bit 7: machine timer interrupt pending

// Interrupt bit plus exception code 7 = machine timer interrupt.
#define MCAUSE_MTI    0x8000000000000007UL

// Timer arm distance ahead of the running mtime. QEMU's virt CLINT
// mtime runs at 10 MHz, so 5000 ticks is 500 us: the pend arrives
// well before the phase-A quiet window starts.
#define TIMER_AHEAD_TICKS 5000UL

// Bounded quiet windows, measured in rdcycle deltas. Plenty of time
// for an enabled pending interrupt to fire (delivery is immediate
// at the next instruction boundary), so a silent window is the
// mechanism, not a slow setup.
#define WINDOW_CYCLES 2000000UL
#define WAIT_BUDGET   10000000UL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

static volatile unsigned long mtie_regs[8];  // trap scratch, mscratch points here
static volatile unsigned long trap_count = 0;
static volatile unsigned long last_mcause = 0;
static volatile unsigned long last_mepc = 0;

static unsigned long read_mie(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mie" : "=r"(v));
    return v;
}

static unsigned long read_mip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mip" : "=r"(v));
    return v;
}

static unsigned long read_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static unsigned long read_cycle(void) {
    unsigned long v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

// Called from the asm trap entry. Disarms the timer first by writing
// mtimecmp to all-ones (a compare above the running mtime clears
// MTIP and is what stops the re-fire), then records what the handler
// observed. Every trap, expected or not, bumps the counter.
void mtie_c_handle(void) {
    unsigned long cause = mtie_regs[2];
    clint_set_mtimecmp(~0UL);
    trap_count++;
    last_mcause = cause;
    last_mepc = mtie_regs[3];
}

extern void mtie_trap_entry(void);

// Poll with a spin budget until the counter reaches want, so a broken
// delivery is a FAIL, not a hang.
static unsigned long wait_for_trap(unsigned long want, unsigned long budget) {
    unsigned long spins = 0;
    while (trap_count < want && spins < budget)
        spins++;
    return spins;
}

// Poll with a spin budget until mip.MTIP reads pending, so a broken
// arm is a FAIL, not a hang.
static unsigned long wait_for_pending(unsigned long budget) {
    unsigned long spins = 0;
    while (((read_mip() & MIP_MTIP) == 0) && spins < budget)
        spins++;
    return spins;
}

// Bounded window measured in rdcycle deltas; returns when the window
// elapses. Used for the phases where zero traps may fire.
static void quiet_window(unsigned long cycles) {
    unsigned long start = read_cycle();
    while (read_cycle() - start < cycles)
        ;
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
    unsigned long mie, mip, mstatus, spins;

    uart_init();
    uart_puts("mie-mtie-gate: mie.MTIE machine timer-interrupt enable-gate check\n");

    // The timer is not armed yet, but disarm it explicitly anyway so
    // the boot-state reads below see a clean pending picture.
    clint_set_mtimecmp(~0UL);

    // 1. Boot state: enable bits clear, timer disarmed before
    // anything runs.
    mie = read_mie();
    mstatus = read_mstatus();
    mip = read_mip();
    uart_puts("boot: mie=");
    uart_put_hex(mie);
    uart_puts(" mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(" mip=");
    uart_put_hex(mip);
    uart_puts(" mtimecmp=");
    uart_put_hex(clint_get_mtimecmp());
    uart_puts("\n");
    check(mie == 0, "mie nonzero at boot");
    check(((mstatus >> 3) & 1UL) == 0, "mstatus.MIE set at boot");
    check((mip & MIP_MTIP) == 0, "mip MTIP pending at boot");
    check(clint_get_mtimecmp() == ~0UL, "mtimecmp not disarmed at boot");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the scratch area. Then set the global MIE bit; it stays set
    // for the whole run so mie.MTIE is the only gating variable.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mtie_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mtie_regs));
    {
        unsigned long tv;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        uart_puts("trap: mtvec=");
        uart_put_hex(tv);
        uart_puts("\n");
        check((tv & ~3UL) == (unsigned long)mtie_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
    }
    __asm__ volatile("csrs mstatus, %0" : : "r"(MSTATUS_MIE));
    mstatus = read_mstatus();
    uart_puts("irq-global: mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(" (stays set for the run)\n");
    check(((mstatus >> 3) & 1UL) == 1, "mstatus.MIE not set");

    // 2. Phase A: MTIE clear. Write the full mie word to 0x0, read
    // it back, and require the write took exactly.
    __asm__ volatile("csrw mie, %0" : : "r"(0UL));
    mie = read_mie();
    uart_puts("phaseA: mie-after-write=");
    uart_put_hex(mie);
    uart_puts(" (expect 0x0)\n");
    check(mie == 0, "mie readback != 0x0 after csrw mie, 0");

    // Arm the CLINT timer a few thousand mtime ticks ahead, then
    // wait for the MTIP bit to pend. MIE is set and MTIP is about
    // to be pending, but MTIE (bit 7) is clear, so no trap can
    // fire: this is the gate under test.
    clint_set_mtimecmp(clint_get_mtime() + TIMER_AHEAD_TICKS);
    spins = wait_for_pending(WAIT_BUDGET);
    check(spins < WAIT_BUDGET, "mip MTIP never pended after arming the timer");
    mip = read_mip();
    uart_puts("phaseA: mip=");
    uart_put_hex(mip);
    uart_puts(" (MTIP bit ");
    uart_puts((mip & MIP_MTIP) ? "SET" : "clear");
    uart_puts(")\n");
    check((mip & MIP_MTIP) != 0, "mip MTIP not pending after timer arm");

    // Quiet window with the interrupt pending, global MIE set, and
    // MTIE clear in mie. Zero traps may fire.
    quiet_window(WINDOW_CYCLES);
    mip = read_mip();
    uart_puts("phaseA: traps-with-MTIE-clear=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 0), mip-after-window=");
    uart_put_hex(mip);
    uart_puts(" (MTIP bit ");
    uart_puts((mip & MIP_MTIP) ? "SET" : "clear");
    uart_puts(")\n");
    check(trap_count == 0, "trap fired while mie.MTIE was clear");
    check((mip & MIP_MTIP) != 0,
          "mip MTIP cleared during phase-A window (pending bit must stay up)");

    // 3. Phase B (control): enable MTIE. The still-pending MTIP
    // must now deliver exactly one machine timer interrupt,
    // proving the phase-A silence was the enable bit.
    __asm__ volatile("csrs mie, %0" : : "r"(MIE_MTIE));
    mie = read_mie();
    uart_puts("phaseB: mie-after-write=");
    uart_put_hex(mie);
    uart_puts(" (expect 0x80)\n");
    check(mie == MIE_MTIE, "mie readback != 0x80 after csrs mie, 0x80");

    spins = wait_for_trap(1, WAIT_BUDGET);
    check(spins < WAIT_BUDGET, "no trap delivered with mie.MTIE set");
    uart_puts("phaseB: mcause=");
    uart_put_hex(last_mcause);
    uart_puts(" mepc=");
    uart_put_hex(last_mepc);
    uart_puts(" trap-count=");
    uart_put_dec(trap_count);
    uart_puts("\n");
    check(last_mcause == MCAUSE_MTI, "trap mcause != 0x8000000000000007");
    check(trap_count == 1, "trap_count != 1 after phase-B enable");

    // The handler disarmed mtimecmp: MTIP must read clear, and the
    // counter must not move during a further quiet window (no
    // re-delivery) with MTIE still set and MIE on.
    mip = read_mip();
    uart_puts("phaseB: mip-after-handler=");
    uart_put_hex(mip);
    uart_puts(" (MTIP bit ");
    uart_puts((mip & MIP_MTIP) ? "SET" : "clear");
    uart_puts(")\n");
    check((mip & MIP_MTIP) == 0, "mip MTIP still set after handler disarm");
    quiet_window(WINDOW_CYCLES);
    uart_puts("phaseC: traps-after-quiet=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 1)\n");
    check(trap_count == 1, "re-delivery fired after timer disarm");

    // The global bit and the mie word must be untouched by the run:
    // MTIE was the only gating variable.
    mstatus = read_mstatus();
    mie = read_mie();
    uart_puts("end: mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(" mie=");
    uart_put_hex(mie);
    uart_puts("\n");
    check(((mstatus >> 3) & 1UL) == 1, "mstatus.MIE changed during the run");
    check(mie == MIE_MTIE, "mie changed during the run");

    uart_puts("RESULT: ");
    uart_puts(fails == 0 ? "PASS" : "FAIL");
    uart_puts(" (traps=");
    uart_put_dec(trap_count);
    uart_puts(")\n");

    // Let the UART drain before touching the finisher device.
    {
        unsigned long drain = clint_get_mtime();
        while (clint_get_mtime() - drain < 100000UL)
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
