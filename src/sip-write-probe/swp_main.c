// swp_main.c: sip WARL write/readback probe (all-ones write).
//
// Mechanism under test: the legalization behavior of the sip CSR on
// a write of all ones. sip is a restricted view of mip: bit 1
// (SSIP, the supervisor software-interrupt pending bit) is the
// software-writable bit, writable through the sip address only while
// mideleg delegates the supervisor software interrupt (bit 1 of
// mideleg); every other visible bit is a read-only alias of the
// corresponding mip bit. The run measures exactly what survives an
// all-ones write, in both delegation states:
//
//   Phase 1 (SSI not delegated at boot): csrw sip, all-ones, read
//   back; then csrw sip, zero, read back. Both readbacks must equal
//   the boot sip baseline (no bit is writable while SSIP is
//   undelegated, and no read-only alias bit is pending), and mip
//   must be unchanged across both writes.
//   Phase 2 (delegate SSI in mideleg, read back mideleg to confirm):
//   csrw sip, all-ones, read back; exactly bit 1 may differ from the
//   boot baseline (it must be set, every other bit unchanged). mip
//   bit 1 must follow the sip write (the alias), with all other mip
//   bits unchanged; this is what makes the non-SSIP sip bits
//   read-only aliases rather than writable bits. Then csrw sip,
//   zero, read back; sip must return byte-identical to the boot
//   baseline and mip bit 1 must clear again.
//   Phase 3: restore mideleg to the boot value and verify mideleg
//   and sip both read back their baselines.
//
// The software interrupt itself is never enabled: mie.MSIE and
// mstatus.MIE read back clear at boot and at the end, so no trap
// can be taken from the asserted pending bit. A trap handler is
// installed anyway (direct-mode mtvec, mscratch scratch area); it
// records mcause/mepc/mtval and counts entries, and the verdict
// requires that count to be 0, so any unexpected trap becomes
// visible instead of silent.
//
// Every check increments the checks counter; a failed check prints
// FAIL and increments the mismatches counter. RESULT: PASS is
// printed only when every check held. The verdict-relevant values
// (boot baselines, every write/readback pair, the mip cross-checks,
// the trap count) feed a 64-bit FNV-1a digest printed as the last
// data line, so the three bench runs can be compared for
// byte-identical output.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.
//
// CSR accesses use direct inline asm. No labels-as-values anywhere,
// so the GCC label-miscompile concern does not arise; the trap
// handler is a plain .S entry point.

#include "../uart.h"

#define MSTATUS_MIE   (1UL << 3)
#define MIE_MSIE      (1UL << 3)
#define SIP_SSIP      (1UL << 1)    // sip bit 1: supervisor software interrupt pending
#define MIDELEG_SSI   (1UL << 1)    // mideleg bit 1: delegate supervisor software interrupt
#define ALL_ONES      (~0UL)

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

extern void swp_trap_entry(void);

// Trap scratch: slot 0 trap count, slot 1 mcause, slot 2 mepc,
// slot 3 mtval, slot 4 parked t1. BSS-cleared to zero by boot.S.
unsigned long swp_regs[5];

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

static void write_sip(unsigned long v) {
    __asm__ volatile("csrw sip, %0" : : "r"(v));
}

static void write_mideleg(unsigned long v) {
    __asm__ volatile("csrw mideleg, %0" : : "r"(v));
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
    unsigned long mie, mstatus, mtvec;
    unsigned long boot_sip, boot_mip, boot_mideleg;
    unsigned long rb, mip_after, mideleg_rb;

    uart_init();
    uart_puts("sip-write-probe: sip all-ones WARL write/readback probe\n");

    // Interrupts must be disabled for the whole run.
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
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)swp_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)swp_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    uart_puts("trap: mtvec=");
    uart_put_hex(mtvec);
    uart_puts("\n");
    check((mtvec & ~3UL) == (unsigned long)swp_trap_entry,
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
    digest64(boot_sip);
    digest64(boot_mip);
    digest64(boot_mideleg);
    check((boot_mideleg & MIDELEG_SSI) == 0,
          "mideleg already delegates SSI at boot; phase 1 needs it clear");

    // Phase 1: SSI not delegated. An all-ones write must legalize to
    // the boot baseline: no sip bit is writable in this state, and
    // the read-only alias bits read whatever mip holds (nothing
    // pending at boot besides M-mode bits, which sip does not
    // expose). A zero write must read back the same baseline.
    write_sip(ALL_ONES);
    rb = read_sip();
    mip_after = read_mip();
    uart_puts("phase1 ones: sip=");
    uart_put_hex(rb);
    uart_puts(" mip=");
    uart_put_hex(mip_after);
    uart_puts("\n");
    digest64(rb);
    digest64(mip_after);
    check(rb == boot_sip,
          "phase 1: all-ones write changed sip although SSI not delegated");
    check(mip_after == boot_mip,
          "phase 1: sip all-ones write changed mip");

    write_sip(0);
    rb = read_sip();
    mip_after = read_mip();
    uart_puts("phase1 zero: sip=");
    uart_put_hex(rb);
    uart_puts(" mip=");
    uart_put_hex(mip_after);
    uart_puts("\n");
    digest64(rb);
    digest64(mip_after);
    check(rb == boot_sip,
          "phase 1: zero write did not read back the boot sip baseline");
    check(mip_after == boot_mip,
          "phase 1: sip zero write changed mip");

    // Phase 2: delegate SSI in mideleg, confirm the delegation took.
    write_mideleg(boot_mideleg | MIDELEG_SSI);
    mideleg_rb = read_mideleg();
    uart_puts("delegated: mideleg=");
    uart_put_hex(mideleg_rb);
    uart_puts("\n");
    digest64(mideleg_rb);
    check((mideleg_rb & MIDELEG_SSI) == MIDELEG_SSI,
          "mideleg.SSI did not take the delegation write");

    // All-ones write with SSIP writable: exactly bit 1 may change
    // relative to the boot baseline. mip bit 1 must follow (the
    // alias), with all other mip bits unchanged: this is what pins
    // the remaining sip bits to read-only aliases rather than
    // writable bits.
    write_sip(ALL_ONES);
    rb = read_sip();
    mip_after = read_mip();
    uart_puts("phase2 ones: sip=");
    uart_put_hex(rb);
    uart_puts(" mip=");
    uart_put_hex(mip_after);
    uart_puts("\n");
    digest64(rb);
    digest64(mip_after);
    check(((rb & ~SIP_SSIP) == (boot_sip & ~SIP_SSIP)) &&
          ((rb & SIP_SSIP) == SIP_SSIP),
          "phase 2: all-ones write did not legalize to exactly SSIP set");
    check((mip_after & SIP_SSIP) == SIP_SSIP,
          "phase 2: mip.SSIP did not follow the sip all-ones write");
    check((mip_after & ~SIP_SSIP) == (boot_mip & ~SIP_SSIP),
          "phase 2: non-SSIP mip bits changed across the sip write");

    // Zero write with SSIP writable: sip must return byte-identical
    // to the boot baseline, and mip bit 1 must clear again.
    write_sip(0);
    rb = read_sip();
    mip_after = read_mip();
    uart_puts("phase2 zero: sip=");
    uart_put_hex(rb);
    uart_puts(" mip=");
    uart_put_hex(mip_after);
    uart_puts("\n");
    digest64(rb);
    digest64(mip_after);
    check(rb == boot_sip,
          "phase 2: zero write did not return sip to the boot baseline");
    check((mip_after & SIP_SSIP) == 0,
          "phase 2: mip.SSIP still set after the sip zero write");
    check((mip_after & ~SIP_SSIP) == (boot_mip & ~SIP_SSIP),
          "phase 2: non-SSIP mip bits changed across the sip zero write");

    // Phase 3: restore mideleg to the boot value and verify both
    // mideleg and sip read back their baselines.
    write_mideleg(boot_mideleg);
    mideleg_rb = read_mideleg();
    rb = read_sip();
    uart_puts("restored: mideleg=");
    uart_put_hex(mideleg_rb);
    uart_puts(" sip=");
    uart_put_hex(rb);
    uart_puts("\n");
    digest64(mideleg_rb);
    digest64(rb);
    check(mideleg_rb == boot_mideleg, "mideleg did not restore to boot value");
    check(rb == boot_sip, "sip baseline changed after mideleg restore");

    // The trap handler must never have fired.
    digest64(swp_regs[0]);
    if (swp_regs[0] != 0) {
        uart_puts("  FAIL: unexpected trap(s), count=");
        uart_put_dec(swp_regs[0]);
        uart_puts(" mcause=");
        uart_put_hex(swp_regs[1]);
        uart_puts(" mepc=");
        uart_put_hex(swp_regs[2]);
        uart_puts(" mtval=");
        uart_put_hex(swp_regs[3]);
        uart_puts("\n");
        checks++;
        mismatches++;
    }
    uart_puts("traps: count=");
    uart_put_dec(swp_regs[0]);
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
        *VIRT_TEST_FINISHER = FINISHER_PASS;
        for (;;) { }
    }
    uart_puts("RESULT: FAIL\n");
    // Park the hart; the harness observes the timeout exit status.
    for (;;) {
        __asm__ volatile("wfi");
    }
}
