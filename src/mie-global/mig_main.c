// mig_main.c: mstatus.MIE global-interrupt-enable gate check (backlog
// item 113).
//
// Mechanism under test: the MIE bit (bit 3) of mstatus gates delivery
// of the machine software interrupt raised by the CLINT msip register,
// independent of the per-source mie.MSIE bit, which stays set for the
// whole run. With MIE clear, an asserted msip leaves the interrupt
// pending in mip but no trap fires; setting MIE delivers exactly one
// machine software interrupt trap, and clearing MIE again re-gates
// delivery of a freshly asserted msip.
//
// Sequence under test:
//   1. Boot: mie reads 0, mstatus.MIE reads 0. Install the trap
//      handler (direct-mode mtvec, mscratch scratch area), set
//      mie.MSIE, and read mie back. mie.MSIE stays set for the rest
//      of the run, so mstatus.MIE is the only gating variable.
//   2. Phase 1 (MIE clear): write mstatus with bit 3 clear via csrc,
//      read back the full word, require bit 3 == 0. Set msip = 1
//      (readback 1), read mip and require the MSIP bit (bit 3) is
//      set: the interrupt is pending. Sit in a bounded quiet
//      window and require trap_count == 0 and mip.MSIP still set.
//      Clear msip (readback 0), mip.MSIP reads 0.
//   3. Phase 2 (MIE set): set msip = 1 first with MIE still clear so
//      the set+readback cannot race the handler, then set MIE via
//      read-modify-csrw of mstatus. Publish the written and
//      read-back full words, require they match with bit 3 set.
//      The trap must fire: wait with a spin budget and require
//      exactly one trap with mcause == 0x8000000000000003
//      (machine software interrupt, code 3). The handler records
//      mcause/mepc/mtval and the mstatus word as seen on trap
//      entry, then clears msip. A further quiet window must leave
//      the count at 1, i.e. no re-delivery after the clear.
//   4. Re-gate: clear bit 3 via csrci mstatus, read back the full
//      word, require bit 3 == 0. Set msip = 1 again (readback 1),
//      quiet window, require trap_count still 1: the mstatus write
//      alone stops delivery. Clear msip.
//
// The machine software interrupt is the only enabled interrupt
// source (mie.MTIE and mie.MEIE are never set), so the mcause value
// and the trap counts are the ground truth for the gate. The trap
// handler clears msip on entry (level-triggered source: the trap
// would re-fire the instant mret runs if msip were still set),
// records mcause/mepc/mtval and the entry mstatus, and bumps the
// trap counter.
//
// CLINT access width: msip for hart 0 is touched as a 32-bit
// register. A 64-bit access was measured to fault on this QEMU
// (load access fault, cause 5, tval 0x02000000); see the defect note
// in src/msip/PROOF.md, backlog item 70.
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
#define MSTATUS_MPIE  (1UL << 7)
#define MSTATUS_MPP   (3UL << 11)
#define MIE_MSIE      (1UL << 3)    // mie bit 3: machine software interrupt enable
#define MIP_MSIP      (1UL << 3)    // mip bit 3: machine software interrupt pending

// Interrupt bit plus exception code 3 = machine software interrupt.
#define MCAUSE_MSI    0x8000000000000003UL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

static volatile unsigned long mig_regs[9];  // trap scratch, mscratch points here
static volatile unsigned long trap_count = 0;
static volatile unsigned long last_mcause = 0;
static volatile unsigned long last_mepc = 0;
static volatile unsigned long last_mtval = 0;
static volatile unsigned long last_entry_mstatus = 0;

static volatile unsigned int *const msip0 = (volatile unsigned int *)CLINT_MSIP0;

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

static unsigned long read_mtime(void) {
    return *(volatile unsigned long *)CLINT_MTIME;
}

// Called from the asm trap entry. Clears msip first (level-triggered
// source), then records what the handler observed, including the
// mstatus word as seen on trap entry.
void mig_c_handle(void) {
    *msip0 = 0;
    trap_count++;
    last_mcause = mig_regs[2];
    last_mepc = mig_regs[3];
    last_mtval = mig_regs[4];
    last_entry_mstatus = mig_regs[8];
}

extern void mig_trap_entry(void);

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
    unsigned long mie, mip, mstatus, spins;

    uart_init();
    uart_puts("mie-global: mstatus.MIE global-interrupt-enable gate check\n");

    // 1. Boot state: both enable bits clear before anything runs.
    mie = read_mie();
    mstatus = read_mstatus();
    uart_puts("boot: mie=");
    uart_put_hex(mie);
    uart_puts(" mstatus=");
    uart_put_hex(mstatus);
    uart_puts(" (MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(")\n");
    check(mie == 0, "mie nonzero at boot");
    check(((mstatus >> 3) & 1UL) == 0, "mstatus.MIE set at boot");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the scratch area. Then set mie.MSIE; it stays set for the
    // whole run so mstatus.MIE is the only gating variable.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mig_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mig_regs));
    {
        unsigned long tv;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        uart_puts("trap: mtvec=");
        uart_put_hex(tv);
        uart_puts("\n");
        check((tv & ~3UL) == (unsigned long)mig_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
    }
    __asm__ volatile("csrs mie, %0" : : "r"(MIE_MSIE));
    mie = read_mie();
    uart_puts("irq-enable: mie-after-set=");
    uart_put_hex(mie);
    uart_puts(" (mie.MSIE=");
    uart_put_dec((mie >> 3) & 1UL);
    uart_puts(", stays set for the run)\n");
    check(mie == MIE_MSIE, "mie readback != 0x8 after csrs mie");

    // 2. Phase 1: MIE clear. Explicitly clear bit 3 with csrc, read
    // back the full mstatus word, and require bit 3 == 0.
    __asm__ volatile("csrc mstatus, %0" : : "r"(MSTATUS_MIE));
    mstatus = read_mstatus();
    uart_puts("phase1: mstatus-after-clear=");
    uart_put_hex(mstatus);
    uart_puts(" (MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(")\n");
    check(((mstatus >> 3) & 1UL) == 0, "mstatus.MIE set after csrc mstatus");

    // Assert msip with the interrupt gated off globally. No trap may
    // fire, but the pending bit must be visible in mip.
    *msip0 = 1;
    uart_puts("phase1: msip-readback=");
    uart_put_dec(*msip0);
    uart_puts("\n");
    check(*msip0 == 1, "msip readback != 1 after set");
    mip = read_mip();
    uart_puts("phase1: mip=");
    uart_put_hex(mip);
    uart_puts(" (MSIP=");
    uart_put_dec((mip >> 3) & 1UL);
    uart_puts(", pending)\n");
    check((mip & MIP_MSIP) != 0, "mip.MSIP not set while msip asserted");
    (void)quiet_poll(2000000UL);
    uart_puts("phase1: traps-with-MIE-clear=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 0)\n");
    check(trap_count == 0, "trap fired while mstatus.MIE was clear");
    mip = read_mip();
    uart_puts("phase1: mip-after-quiet=");
    uart_put_hex(mip);
    uart_puts(" (MSIP=");
    uart_put_dec((mip >> 3) & 1UL);
    uart_puts(", still pending)\n");
    check((mip & MIP_MSIP) != 0, "mip.MSIP cleared during the quiet window");
    *msip0 = 0;
    uart_puts("phase1: msip-after-clear=");
    uart_put_dec(*msip0);
    uart_puts(" mip.MSIP=");
    uart_put_dec((read_mip() >> 3) & 1UL);
    uart_puts("\n");
    check(*msip0 == 0, "msip readback != 0 after clear");
    check((read_mip() & MIP_MSIP) == 0, "mip.MSIP still set after msip clear");

    // 3. Phase 2: MIE set. Set msip first while MIE is still clear,
    // so the set+readback cannot race the handler; then set MIE via
    // read-modify-csrw of mstatus. The trap must fire on the csrw.
    *msip0 = 1;
    {
        unsigned long rb = *msip0;
        uart_puts("phase2: msip-readback=");
        uart_put_dec(rb);
        uart_puts(" mip.MSIP=");
        uart_put_dec((read_mip() >> 3) & 1UL);
        uart_puts("\n");
        check(rb == 1, "msip readback != 1 on phase-2 set");
    }
    {
        unsigned long written = read_mstatus() | MSTATUS_MIE;
        __asm__ volatile("csrw mstatus, %0" : : "r"(written));
        mstatus = read_mstatus();
        uart_puts("phase2: mstatus-written=");
        uart_put_hex(written);
        uart_puts(" mstatus-readback=");
        uart_put_hex(mstatus);
        uart_puts(" (MIE=");
        uart_put_dec((mstatus >> 3) & 1UL);
        uart_puts(")\n");
        // The pending interrupt traps between the csrw and this
        // readback: hardware moves MIE into MPIE on trap entry and
        // mret sets MPIE=1 on return, so the readback may carry the
        // MPIE bit even though the write did not set it. Require the
        // write took modulo MPIE, and MIE itself is set.
        check((mstatus & ~MSTATUS_MPIE) == written,
              "mstatus readback != written value (modulo MPIE)");
        check(((mstatus >> 3) & 1UL) == 1, "mstatus.MIE not set after csrw");
    }
    spins = wait_for_trap(1, 10000000UL);
    uart_puts("phase2: spins-to-trap=");
    uart_put_dec(spins);
    uart_puts("\n");
    check(spins < 10000000UL, "no trap delivered with mstatus.MIE set");
    uart_puts("phase2: mcause=");
    uart_put_hex(last_mcause);
    uart_puts(" mepc=");
    uart_put_hex(last_mepc);
    uart_puts(" mtval=");
    uart_put_hex(last_mtval);
    uart_puts(" trap-count=");
    uart_put_dec(trap_count);
    uart_puts("\n");
    check(last_mcause == MCAUSE_MSI, "trap mcause != 0x8000000000000003");
    check(trap_count == 1, "trap_count != 1 after phase-2 set");
    uart_puts("phase2: mstatus-on-trap-entry=");
    uart_put_hex(last_entry_mstatus);
    uart_puts(" (MIE=");
    uart_put_dec((last_entry_mstatus >> 3) & 1UL);
    uart_puts(" MPIE=");
    uart_put_dec((last_entry_mstatus >> 7) & 1UL);
    uart_puts(" MPP=");
    uart_put_dec((last_entry_mstatus >> 11) & 3UL);
    uart_puts(")\n");
    check(((last_entry_mstatus >> 3) & 1UL) == 0,
          "entry mstatus.MIE not auto-cleared");
    check(((last_entry_mstatus >> 7) & 1UL) == 1,
          "entry mstatus.MPIE != 1");
    check(((last_entry_mstatus >> 11) & 3UL) == 3,
          "entry mstatus.MPP != M-mode");

    // The handler cleared msip: read 0, the pending bit is gone from
    // mip, and the counter must not move during a further quiet
    // window (no re-delivery).
    uart_puts("phase2: msip-after-handler=");
    uart_put_dec(*msip0);
    uart_puts(" mip.MSIP=");
    uart_put_dec((read_mip() >> 3) & 1UL);
    uart_puts("\n");
    check(*msip0 == 0, "msip not cleared by the trap handler");
    check((read_mip() & MIP_MSIP) == 0, "mip.MSIP still set after handler");
    (void)quiet_poll(2000000UL);
    uart_puts("phase2: traps-after-quiet=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 1)\n");
    check(trap_count == 1, "re-delivery fired after msip cleared");

    // 4. Re-gate: clear MIE again with csrci, read back the full
    // word, and prove a freshly asserted msip no longer traps. The
    // mstatus write alone stops delivery; mie.MSIE is untouched.
    __asm__ volatile("csrci mstatus, 8");
    mstatus = read_mstatus();
    uart_puts("regate: mstatus-after-clear=");
    uart_put_hex(mstatus);
    uart_puts(" (MIE=");
    uart_put_dec((mstatus >> 3) & 1UL);
    uart_puts(")\n");
    check(((mstatus >> 3) & 1UL) == 0, "mstatus.MIE set after csrci mstatus");
    *msip0 = 1;
    uart_puts("regate: msip-readback=");
    uart_put_dec(*msip0);
    uart_puts(" mip.MSIP=");
    uart_put_dec((read_mip() >> 3) & 1UL);
    uart_puts("\n");
    check(*msip0 == 1, "msip readback != 1 on re-gate set");
    check((read_mip() & MIP_MSIP) != 0, "mip.MSIP not set on re-gate");
    (void)quiet_poll(2000000UL);
    uart_puts("regate: traps-with-MIE-clear=");
    uart_put_dec(trap_count);
    uart_puts(" (expect 1)\n");
    check(trap_count == 1, "trap fired after mstatus.MIE was cleared again");
    *msip0 = 0;
    check(*msip0 == 0, "msip readback != 0 after final clear");

    // mie.MSIE must be untouched: mstatus.MIE was the only gating
    // variable for the whole run.
    mie = read_mie();
    uart_puts("end: mie=");
    uart_put_hex(mie);
    uart_puts(" mstatus=");
    uart_put_hex(read_mstatus());
    uart_puts("\n");
    check(mie == MIE_MSIE, "mie changed during the run");

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
