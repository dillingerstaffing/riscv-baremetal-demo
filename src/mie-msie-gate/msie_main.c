// msie_main.c: mie.MSIE machine software-interrupt enable-gate check
// (backlog item "riscv mie-msie-gate").
//
// Mechanism under test: mie.MSIE (bit 3) is the M-mode enable bit
// for the machine software interrupt, independent of the pending
// bit. With mstatus.MIE set and the CLINT msip pended for hart 0
// while MSIE is clear, the pending bit must sit pending with zero
// traps; setting MSIE must then deliver exactly one trap. This is
// the enable-bit complement to the shipped src/mip-msip (which
// gated delivery on MIE and MSIE together) and src/mip-pending-
// no-trap (which measured the pending reflection with the enable
// off); here MSIE itself is the only gating variable.
//
// Sequence under test:
//   1. Boot: mie reads 0, mstatus.MIE reads 0, the CLINT msip MMIO
//      reads 0. Disarm the CLINT timer (it is not part of this
//      mechanism), install the trap handler (direct-mode mtvec,
//      mscratch scratch area), set mstatus.MIE, and read everything
//      back. MIE stays set for the whole run.
//   2. Phase A (MSIE clear): write mie = 0x0 with csrw, read it
//      back. Pend the CLINT msip with a 32-bit MMIO write of 1;
//      read the MMIO back and require 1, then require mip.MSIP
//      (bit 3) reads pending. Spin a bounded window (2,000,000
//      rdcycle deltas) and require trap_count == 0 and mip.MSIP
//      still pending, i.e. the interrupt sat pending and
//      globally enabled yet never delivered.
//   3. Phase B (control): set MSIE with csrs mie, 8, read back
//      mie == 0x8. The still-pending msip must now trap exactly
//      once: wait for trap_count == 1 and require
//      mcause == 0x8000000000000003 (machine software interrupt,
//      code 3). The handler clears the CLINT msip inside the trap
//      (level-triggered source: clearing first is what stops the
//      re-fire), records mcause/mepc, and bumps the counter.
//   4. Phase C (quiet window): another bounded rdcycle window must
//      leave the count at 1, i.e. no re-delivery after the clear.
//
// The machine timer interrupt is never armed and no S-mode source
// is ever programmed, so the machine software interrupt is the
// only source that can ever be taken; the mcause value plus the
// trap counts are the ground truth for the gate.
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
#define MIE_MSIE      (1UL << 3)    // mie bit 3: machine software interrupt enable
#define MIP_MSIP      (1UL << 3)    // mip bit 3: machine software interrupt pending

// Interrupt bit plus exception code 3 = machine software interrupt.
#define MCAUSE_MSI    0x8000000000000003UL

// QEMU virt CLINT base; msip for hart 0 is a 32-bit register at the
// base (32-bit access form; the 64-bit fault noted in src/msip/
// applies to this register, so this module never uses 64-bit
// accesses on it).
#define CLINT_MSIP0   0x02000000UL

// Bounded quiet windows, measured in rdcycle deltas. Plenty of time
// for an enabled pending interrupt to fire (delivery is immediate
// at the next instruction boundary), so a silent window is the
// mechanism, not a slow setup.
#define WINDOW_CYCLES 2000000UL
#define WAIT_BUDGET   10000000UL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

static volatile unsigned int *const msip0 = (volatile unsigned int *)CLINT_MSIP0;

static volatile unsigned long msie_regs[8];  // trap scratch, mscratch points here
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

// Called from the asm trap entry. Clears the CLINT msip first
// (level-triggered source), then records what the handler observed.
// Every trap, expected or not, bumps the counter.
void msie_c_handle(void) {
    unsigned long cause = msie_regs[2];
    *msip0 = 0;
    trap_count++;
    last_mcause = cause;
    last_mepc = msie_regs[3];
}

extern void msie_trap_entry(void);

// Poll with a spin budget until the counter reaches want, so a broken
// delivery is a FAIL, not a hang.
static unsigned long wait_for_trap(unsigned long want, unsigned long budget) {
    unsigned long spins = 0;
    while (trap_count < want && spins < budget)
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
    uart_puts("mie-msie-gate: mie.MSIE machine software-interrupt enable-gate check\n");

    // The machine timer is not part of this mechanism; disarm it so
    // no stray MTIP can confuse the pending picture.
    clint_set_mtimecmp(~0UL);

    // 1. Boot state: enable bits clear, msip MMIO clear before
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
    uart_puts(" msip-mmio=");
    uart_put_dec(*msip0);
    uart_puts("\n");
    check(mie == 0, "mie nonzero at boot");
    check(((mstatus >> 3) & 1UL) == 0, "mstatus.MIE set at boot");
    check((mip & MIP_MSIP) == 0, "mip MSIP pending at boot");
    check(*msip0 == 0, "msip MMIO nonzero at boot");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the scratch area. Then set the global MIE bit; it stays set
    // for the whole run so mie.MSIE is the only gating variable.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)msie_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)msie_regs));
    {
        unsigned long tv;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        uart_puts("trap: mtvec=");
        uart_put_hex(tv);
        uart_puts("\n");
        check((tv & ~3UL) == (unsigned long)msie_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
    }
    __asm__ volatile("csrs mstatus, %0" : : "r"(MSTATUS_MIE));
    mstatus = read_mstatus();
    uart_puts("irq-global: mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(" (stays set for the run)\n");
    check(((mstatus >> 3) & 1UL) == 1, "mstatus.MIE not set");

    // 2. Phase A: MSIE clear. Write the full mie word to 0x0, read
    // it back, and require the write took exactly.
    __asm__ volatile("csrw mie, %0" : : "r"(0UL));
    mie = read_mie();
    uart_puts("phaseA: mie-after-write=");
    uart_put_hex(mie);
    uart_puts(" (expect 0x0)\n");
    check(mie == 0, "mie readback != 0x0 after csrw mie, 0");

    // Pend the software interrupt via the CLINT msip MMIO. MIE is
    // set and MSIP is about to be pending, but MSIE (bit 3) is
    // clear, so no trap can fire: this is the gate under test.
    *msip0 = 1;
    uart_puts("phaseA: msip-mmio-after-set=");
    uart_put_dec(*msip0);
    uart_puts(" (expect 1)\n");
    check(*msip0 == 1, "msip MMIO did not read back 1 after set");
    mip = read_mip();
    uart_puts("phaseA: mip=");
    uart_put_hex(mip);
    uart_puts(" (MSIP bit ");
    uart_puts((mip & MIP_MSIP) ? "SET" : "clear");
    uart_puts(")\n");
    check((mip & MIP_MSIP) != 0, "mip MSIP not pending after msip set");

    // Quiet window with the interrupt pending, global MIE set, and
    // MSIE clear in mie. Zero traps may fire.
    quiet_window(WINDOW_CYCLES);
    mip = read_mip();
    uart_puts("phaseA: traps-with-MSIE-clear=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 0), mip-after-window=");
    uart_put_hex(mip);
    uart_puts(" (MSIP bit ");
    uart_puts((mip & MIP_MSIP) ? "SET" : "clear");
    uart_puts(")\n");
    check(trap_count == 0, "trap fired while mie.MSIE was clear");
    check((mip & MIP_MSIP) != 0,
          "mip MSIP cleared during phase-A window (pending bit must stay up)");

    // 3. Phase B (control): enable MSIE. The still-pending msip
    // must now deliver exactly one machine software interrupt,
    // proving the phase-A silence was the enable bit.
    __asm__ volatile("csrs mie, %0" : : "r"(MIE_MSIE));
    mie = read_mie();
    uart_puts("phaseB: mie-after-write=");
    uart_put_hex(mie);
    uart_puts(" (expect 0x8)\n");
    check(mie == MIE_MSIE, "mie readback != 0x8 after csrs mie, 8");

    spins = wait_for_trap(1, WAIT_BUDGET);
    check(spins < WAIT_BUDGET, "no trap delivered with mie.MSIE set");
    uart_puts("phaseB: mcause=");
    uart_put_hex(last_mcause);
    uart_puts(" mepc=");
    uart_put_hex(last_mepc);
    uart_puts(" trap-count=");
    uart_put_dec(trap_count);
    uart_puts("\n");
    check(last_mcause == MCAUSE_MSI, "trap mcause != 0x8000000000000003");
    check(trap_count == 1, "trap_count != 1 after phase-B enable");

    // The handler cleared msip: MSIP must read clear, and the
    // counter must not move during a further quiet window (no
    // re-delivery) with MSIE still set and MIE on.
    mip = read_mip();
    uart_puts("phaseB: mip-after-handler=");
    uart_put_hex(mip);
    uart_puts(" (MSIP bit ");
    uart_puts((mip & MIP_MSIP) ? "SET" : "clear");
    uart_puts(")\n");
    check((mip & MIP_MSIP) == 0, "mip MSIP still set after handler clear");
    quiet_window(WINDOW_CYCLES);
    uart_puts("phaseC: traps-after-quiet=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 1)\n");
    check(trap_count == 1, "re-delivery fired after msip clear");

    // The global bit and the mie word must be untouched by the run:
    // MSIE was the only gating variable.
    mstatus = read_mstatus();
    mie = read_mie();
    uart_puts("end: mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(" mie=");
    uart_put_hex(mie);
    uart_puts("\n");
    check(((mstatus >> 3) & 1UL) == 1, "mstatus.MIE changed during the run");
    check(mie == MIE_MSIE, "mie changed during the run");

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
