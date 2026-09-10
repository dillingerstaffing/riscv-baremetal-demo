// stie_main.c: mie.STIE vs mie.MTIE enable-bit separation check
// (backlog item 149).
//
// Mechanism under test: each bit of the mie CSR gates delivery of
// one specific interrupt source, independent of the other bits.
// With mstatus.MIE set and a machine timer interrupt pending in
// mip.MTIP, setting mie to 0x20 (STIE, the supervisor timer enable,
// bit 5) must deliver nothing, because the pending source's own bit
// (MTIE, bit 7) is clear. Setting mie to 0xA0 (MTIE|STIE) must then
// deliver exactly one machine timer interrupt, proving the pending
// interrupt was real and the earlier silence was the enable bit,
// not a broken setup. This is the enable-bit separation complement
// to the shipped mie-msip (MSIE gate) and mie-global (MIE gate)
// modules.
//
// Sequence under test:
//   1. Boot: mie reads 0, mstatus.MIE reads 0. Disarm the timer,
//      install the trap handler (direct-mode mtvec, mscratch
//      scratch area), set mstatus.MIE, and read everything back.
//      MIE stays set for the whole run, so the mie bits are the
//      only gating variables.
//   2. Phase 1 (STIE only): write mie = 0x20 with csrw, read back
//      the full word, require mie == 0x20. Arm the CLINT timer
//      mtimecmp = mtime + STIE_AHEAD_TICKS, poll mip until the MTIP
//      bit (bit 7) reads pending, then spin a bounded quiet window
//      and require trap_count == 0. Read mip back: MTIP must still
//      read pending (0x80), i.e. the interrupt sat pending and
//      enabled-globally yet never delivered.
//   3. Phase 2 (control): write mie = 0xA0 (MTIE|STIE) with csrw,
//      read back, require mie == 0xA0. The still-pending MTIP must
//      now trap exactly once: wait for trap_count == 1 and require
//      mcause == 0x8000000000000007 (machine timer interrupt,
//      code 7). The handler disarms the timer inside the trap
//      (level-triggered source), records mcause/mepc, and bumps the
//      counter. A further quiet window must leave the count at 1,
//      i.e. no re-delivery after the disarm.
//
// No S-mode timer source is ever programmed, so the STIE bit enables
// nothing that can fire; the only interrupt that can ever be taken
// is the machine timer interrupt, and the mcause value plus the trap
// counts are the ground truth for the gate.
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
#define MIE_STIE      (1UL << 5)    // mie bit 5: supervisor timer interrupt enable
#define MIE_MTIE      (1UL << 7)    // mie bit 7: machine timer interrupt enable
#define MIP_MTIP      (1UL << 7)    // mip bit 7: machine timer interrupt pending

// Interrupt bit plus exception code 7 = machine timer interrupt.
#define MCAUSE_MTI    0x8000000000000007UL

// Timer arming: mtimecmp = mtime + 1000 ticks = 100 us at the 10 MHz
// CLINT timebase; the MTIP poll below waits for the pending bit, so
// the exact lead is not load-bearing.
#define STIE_AHEAD_TICKS 1000UL
#define QUIET_SPINS 2000000UL
#define WAIT_BUDGET 10000000UL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

static volatile unsigned long stie_regs[8];  // trap scratch, mscratch points here
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

// Called from the asm trap entry. Disarms the timer first
// (level-triggered source), then records what the handler observed.
// Every trap, expected or not, bumps the counter.
void stie_c_handle(void) {
    unsigned long cause = stie_regs[2];
    clint_set_mtimecmp(~0UL);
    trap_count++;
    last_mcause = cause;
    last_mepc = stie_regs[3];
}

extern void stie_trap_entry(void);

// Poll with a spin budget until the counter reaches want, so a broken
// delivery is a FAIL, not a hang.
static unsigned long wait_for_trap(unsigned long want, unsigned long budget) {
    unsigned long spins = 0;
    while (trap_count < want && spins < budget)
        spins++;
    return spins;
}

// Poll with a spin budget until the mip MTIP bit reads pending, so a
// timer that never expires is a FAIL, not a hang.
static unsigned long wait_for_mtip(unsigned long budget) {
    unsigned long spins = 0;
    while (((read_mip() & MIP_MTIP) == 0) && spins < budget)
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
    unsigned long mie, mip, mstatus, spins;

    uart_init();
    uart_puts("mie-stie: mie.STIE vs mie.MTIE enable-bit separation check\n");

    // Disarm the timer before anything can go pending.
    clint_set_mtimecmp(~0UL);

    // 1. Boot state: both enable bits clear before anything runs.
    mie = read_mie();
    mstatus = read_mstatus();
    mip = read_mip();
    uart_puts("boot: mie=");
    uart_put_hex(mie);
    uart_puts(" mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(" mip=");
    uart_put_hex(mip);
    uart_puts("\n");
    check(mie == 0, "mie nonzero at boot");
    check(((mstatus >> 3) & 1UL) == 0, "mstatus.MIE set at boot");
    check((mip & MIP_MTIP) == 0, "mip MTIP pending at boot after disarm");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the scratch area. Then set the global MIE bit; it stays set
    // for the whole run so the mie bits are the only gating variables.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)stie_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)stie_regs));
    {
        unsigned long tv;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        uart_puts("trap: mtvec=");
        uart_put_hex(tv);
        uart_puts("\n");
        check((tv & ~3UL) == (unsigned long)stie_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
    }
    __asm__ volatile("csrs mstatus, %0" : : "r"(MSTATUS_MIE));
    mstatus = read_mstatus();
    uart_puts("irq-global: mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(" (stays set for the run)\n");
    check(((mstatus >> 3) & 1UL) == 1, "mstatus.MIE not set");

    // 2. Phase 1: STIE only. Write the full mie word to 0x20, read it
    // back, and require the write took exactly.
    __asm__ volatile("csrw mie, %0" : : "r"(MIE_STIE));
    mie = read_mie();
    uart_puts("phase1: mie-after-write=");
    uart_put_hex(mie);
    uart_puts(" (expect 0x20)\n");
    check(mie == MIE_STIE, "mie readback != 0x20 after csrw mie, 0x20");

    // Arm the machine timer interrupt and wait until mip shows it
    // pending. MIE is set and MTIP is about to be pending, but MTIE
    // (bit 7) is clear, so no trap can fire: this is the gate under
    // test. No S-mode timer source is programmed, so STIE enables
    // nothing that can fire.
    clint_set_mtimecmp(clint_get_mtime() + STIE_AHEAD_TICKS);
    spins = wait_for_mtip(WAIT_BUDGET);
    mip = read_mip();
    uart_puts("phase1: spins-to-MTIP-pending=");
    uart_put_dec(spins);
    uart_puts(" mip=");
    uart_put_hex(mip);
    uart_puts(" (MTIP bit ");
    uart_puts((mip & MIP_MTIP) ? "SET" : "clear");
    uart_puts(")\n");
    check(spins < WAIT_BUDGET, "MTIP never went pending after arming");
    check((mip & MIP_MTIP) != 0, "mip MTIP not pending after arm window");

    // Quiet window with the interrupt pending, global MIE set, and
    // only STIE enabled in mie. Zero traps may fire.
    (void)quiet_poll(QUIET_SPINS);
    mip = read_mip();
    uart_puts("phase1: traps-with-STIE-only=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 0), mip-after-window=");
    uart_put_hex(mip);
    uart_puts(" (MTIP bit ");
    uart_puts((mip & MIP_MTIP) ? "SET" : "clear");
    uart_puts(")\n");
    check(trap_count == 0, "trap fired while only mie.STIE was set");
    check((mip & MIP_MTIP) != 0,
          "mip MTIP cleared during phase-1 window (pending bit must stay up)");

    // 3. Phase 2 (control): enable MTIE alongside STIE. The
    // still-pending MTIP must now deliver exactly one machine timer
    // interrupt, proving the phase-1 silence was the enable bit.
    __asm__ volatile("csrw mie, %0" : : "r"(MIE_STIE | MIE_MTIE));
    mie = read_mie();
    uart_puts("phase2: mie-after-write=");
    uart_put_hex(mie);
    uart_puts(" (expect 0xa0)\n");
    check(mie == (MIE_STIE | MIE_MTIE),
          "mie readback != 0xa0 after csrw mie, 0xa0");

    spins = wait_for_trap(1, WAIT_BUDGET);
    uart_puts("phase2: spins-to-trap=");
    uart_put_dec(spins);
    uart_puts("\n");
    check(spins < WAIT_BUDGET, "no trap delivered with mie.MTIE set");
    uart_puts("phase2: mcause=");
    uart_put_hex(last_mcause);
    uart_puts(" mepc=");
    uart_put_hex(last_mepc);
    uart_puts(" trap-count=");
    uart_put_dec(trap_count);
    uart_puts("\n");
    check(last_mcause == MCAUSE_MTI, "trap mcause != 0x8000000000000007");
    check(trap_count == 1, "trap_count != 1 after phase-2 enable");

    // The handler disarmed the timer: MTIP must read clear, and the
    // counter must not move during a further quiet window (no
    // re-delivery) with both enable bits still set and MIE on.
    mip = read_mip();
    uart_puts("phase2: mip-after-handler=");
    uart_put_hex(mip);
    uart_puts(" (MTIP bit ");
    uart_puts((mip & MIP_MTIP) ? "SET" : "clear");
    uart_puts(")\n");
    check((mip & MIP_MTIP) == 0, "mip MTIP still set after handler disarm");
    (void)quiet_poll(QUIET_SPINS);
    uart_puts("phase2: traps-after-quiet=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 1)\n");
    check(trap_count == 1, "re-delivery fired after timer disarm");

    // The global bit and the mie word must be untouched by the run:
    // the two enable bits were the only gating variables.
    mstatus = read_mstatus();
    mie = read_mie();
    uart_puts("end: mstatus.MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(" mie=");
    uart_put_hex(mie);
    uart_puts("\n");
    check(((mstatus >> 3) & 1UL) == 1, "mstatus.MIE changed during the run");
    check(mie == (MIE_STIE | MIE_MTIE), "mie changed during the run");

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
