// sip_main.c: sip SSIP (bit 1) delegation-gated write/readback check.
//
// Mechanism under test: the supervisor software-interrupt pending
// bit of the sip CSR, bit 1 (SSIP). On this machine sip is a
// restricted view of mip: bit 1 is writable through the sip address
// only while mideleg delegates the supervisor software interrupt
// (bit 1 of mideleg); when it is not delegated, the bit reads as
// zero and writes through sip are dropped. The run verifies both
// halves of that rule against the CSR readbacks:
//
//   Phase A (SSIP not delegated at boot): csrs sip, bit 1, read
//   back; the write must be dropped, sip still reads the boot
//   baseline, and no other sip bit may change.
//   Phase B (delegate bit 1 in mideleg, read back mideleg to confirm
//   the delegation took): csrs sip, bit 1, read back; bit 1 must be
//   set and every other sip bit unchanged. The mip CSR is read on
//   the same transitions: mip bit 1 must follow sip bit 1, with all
//   other mip bits unchanged. Then csrc sip, bit 1, read back; sip
//   must return byte-identical to the boot baseline and mip bit 1
//   must clear again.
//   Phase C: restore mideleg to the boot value, read back both
//   mideleg and sip, and verify sip reads the baseline again.
//
// The software interrupt itself is never enabled: mie.MSIE and
// mstatus.MIE are read back clear at boot and at the end, so no
// trap can be taken from the asserted pending bit. A trap handler
// is installed anyway (direct-mode mtvec, mscratch scratch area);
// it records mcause/mepc/mtval and counts entries, and the verdict
// requires that count to be 0, so any unexpected trap becomes
// visible instead of silent.
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

#include "../uart.h"

#define MSTATUS_MIE   (1UL << 3)
#define MIE_MSIE      (1UL << 3)
#define SIP_SSIP      (1UL << 1)    // sip bit 1: supervisor software interrupt pending
#define MIDELEG_SSI   (1UL << 1)    // mideleg bit 1: delegate supervisor software interrupt

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

extern void sip_trap_entry(void);

// Trap scratch: recorded mcause, mepc, mtval, and trap count.
// BSS-cleared to zero by boot.S.
unsigned long sip_regs[4];

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

static unsigned long read_sip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, sip" : "=r"(v));
    return v;
}

static unsigned long read_mip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mip" : "=r"(v));
    return v;
}

static unsigned long read_mideleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mideleg" : "=r"(v));
    return v;
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
    unsigned long mie, mstatus, mtvec;
    unsigned long boot_sip, boot_mip, boot_mideleg;
    unsigned long sip_rb, mip_rb, mideleg_rb;

    uart_init();
    uart_puts("sip-ssip: sip SSIP (bit 1) delegation-gated write/readback check\n");

    // Interrupts must be disabled for the whole run; with the pending
    // bit asserted and no enable, nothing can be taken.
    mie = read_mie();
    mstatus = read_mstatus();
    uart_puts("boot: mie=");
    uart_put_hex(mie);
    uart_puts(" mstatus=");
    uart_put_hex(mstatus);
    uart_puts("\n");
    check((mie & MIE_MSIE) == 0, "mie.MSIE set at boot");
    check((mstatus & MSTATUS_MIE) == 0, "mstatus.MIE set at boot");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the recording area.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)sip_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)sip_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    uart_puts("trap: mtvec=");
    uart_put_hex(mtvec);
    uart_puts("\n");
    check((mtvec & ~3UL) == (unsigned long)sip_trap_entry,
          "mtvec did not take the handler address");
    check((mtvec & 3UL) == 0, "mtvec not in direct mode");

    // Baselines at boot.
    boot_sip = read_sip();
    boot_mip = read_mip();
    boot_mideleg = read_mideleg();
    uart_puts("boot: sip=");
    uart_put_hex(boot_sip);
    uart_puts(" mip=");
    uart_put_hex(boot_mip);
    uart_puts(" mideleg=");
    uart_put_hex(boot_mideleg);
    uart_puts(" (SSI delegated: ");
    uart_put_dec((boot_mideleg & MIDELEG_SSI) ? 1UL : 0UL);
    uart_puts(")\n");
    check((boot_mideleg & MIDELEG_SSI) == 0,
          "mideleg already delegates SSI at boot; phase A needs it clear");

    // Phase A: SSIP not delegated. The csrs write must be dropped:
    // sip still reads the baseline and no other bit may change.
    __asm__ volatile("csrs sip, %0" : : "r"(SIP_SSIP));
    sip_rb = read_sip();
    mip_rb = read_mip();
    uart_puts("phaseA set (not delegated): sip=");
    uart_put_hex(sip_rb);
    uart_puts(" mip=");
    uart_put_hex(mip_rb);
    uart_puts("\n");
    check(sip_rb == boot_sip,
          "phase A: sip changed although SSIP not delegated");
    check((mip_rb & SIP_SSIP) == 0,
          "phase A: mip.SSIP set although write went through sip undelegated");

    // Phase B: delegate the supervisor software interrupt, confirm
    // the delegation took in the readback.
    __asm__ volatile("csrw mideleg, %0" : : "r"(boot_mideleg | MIDELEG_SSI));
    mideleg_rb = read_mideleg();
    uart_puts("delegated: mideleg=");
    uart_put_hex(mideleg_rb);
    uart_puts("\n");
    check((mideleg_rb & MIDELEG_SSI) == MIDELEG_SSI,
          "mideleg.SSI did not take the delegation write");

    // Set bit 1 through sip. sip bit 1 must set with all other sip
    // bits unchanged; mip bit 1 must follow, other mip bits unchanged.
    __asm__ volatile("csrs sip, %0" : : "r"(SIP_SSIP));
    sip_rb = read_sip();
    mip_rb = read_mip();
    uart_puts("phaseB set (delegated): sip=");
    uart_put_hex(sip_rb);
    uart_puts(" (SSIP=");
    uart_put_dec((sip_rb & SIP_SSIP) ? 1UL : 0UL);
    uart_puts(") mip=");
    uart_put_hex(mip_rb);
    uart_puts("\n");
    check((sip_rb & SIP_SSIP) == SIP_SSIP,
          "phase B: sip.SSIP not set after csrs bit 1 while delegated");
    check((sip_rb & ~SIP_SSIP) == (boot_sip & ~SIP_SSIP),
          "phase B: non-SSIP sip bits changed across the set transition");
    check((mip_rb & SIP_SSIP) == SIP_SSIP,
          "phase B: mip.SSIP did not follow the sip write");
    check((mip_rb & ~SIP_SSIP) == (boot_mip & ~SIP_SSIP),
          "phase B: non-SSIP mip bits changed across the set transition");

    // Clear bit 1 through sip. sip must return byte-identical to the
    // boot baseline, mip bit 1 must clear with other bits unchanged.
    __asm__ volatile("csrc sip, %0" : : "r"(SIP_SSIP));
    sip_rb = read_sip();
    mip_rb = read_mip();
    uart_puts("phaseB clear (delegated): sip=");
    uart_put_hex(sip_rb);
    uart_puts(" mip=");
    uart_put_hex(mip_rb);
    uart_puts("\n");
    check(sip_rb == boot_sip,
          "phase B: sip did not return to boot baseline after csrc bit 1");
    check((mip_rb & SIP_SSIP) == 0,
          "phase B: mip.SSIP still set after csrc bit 1");
    check((mip_rb & ~SIP_SSIP) == (boot_mip & ~SIP_SSIP),
          "phase B: non-SSIP mip bits changed across the clear transition");

    // Phase C: restore mideleg to the boot value and verify the
    // delegation is gone and sip reads the baseline again.
    __asm__ volatile("csrw mideleg, %0" : : "r"(boot_mideleg));
    mideleg_rb = read_mideleg();
    sip_rb = read_sip();
    uart_puts("restored: mideleg=");
    uart_put_hex(mideleg_rb);
    uart_puts(" sip=");
    uart_put_hex(sip_rb);
    uart_puts("\n");
    check(mideleg_rb == boot_mideleg, "mideleg did not restore to boot value");
    check(sip_rb == boot_sip, "sip baseline changed after mideleg restore");

    // The trap handler must never have fired.
    if (sip_regs[3] != 0) {
        uart_puts("  FAIL: unexpected trap(s), count=");
        uart_put_dec(sip_regs[3]);
        uart_puts(" mcause=");
        uart_put_hex(sip_regs[0]);
        uart_puts(" mepc=");
        uart_put_hex(sip_regs[1]);
        uart_puts(" mtval=");
        uart_put_hex(sip_regs[2]);
        uart_puts("\n");
        fails++;
    }
    uart_puts("traps: count=");
    uart_put_dec(sip_regs[3]);
    uart_puts("\n");

    // Enables must still be clear at the end.
    mie = read_mie();
    mstatus = read_mstatus();
    uart_puts("end: mie=");
    uart_put_hex(mie);
    uart_puts(" mstatus=");
    uart_put_hex(mstatus);
    uart_puts("\n");
    check((mie & MIE_MSIE) == 0, "mie.MSIE changed during run");
    check((mstatus & MSTATUS_MIE) == 0, "mstatus.MIE changed during run");

    if (fails == 0) {
        uart_puts("RESULT: PASS\n");
        *VIRT_TEST_FINISHER = FINISHER_PASS;
        for (;;) { }
    }
    uart_puts("RESULT: FAIL\n");
    // Park the hart; the harness observes the timeout exit status.
    for (;;) {
        __asm__ volatile("wfi");
    }
}
