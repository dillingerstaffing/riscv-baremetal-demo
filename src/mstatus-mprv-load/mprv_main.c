// mprv_main.c: mstatus.MPRV makes M-mode loads honor mstatus.MPP
// (backlog item "riscv mstatus-mprv-load").
//
// Mechanism under test: mstatus.MPRV (bit 17). When MPRV=1, explicit
// M-mode data loads and stores are privilege-checked as if issued
// by the mode in mstatus.MPP instead of M-mode (privileged spec,
// section 3.1.6). The experiment installs one unlocked PMP TOR
// entry covering a 4 KiB test page with R=W=X=0: unlocked entries
// do not apply to M-mode, so M-mode loads from the page succeed,
// but the same page checked as a U-mode access faults. Then:
//
// Phase A (control): MPRV=0, MPP=U. A plain M-mode load from the
//   test page must succeed and return the canary.
// Phase B (probe): MPRV=1, MPP=U. The same load must raise a load
//   access fault (mcause=0x5) with mepc exactly at the load site
//   and mtval equal to the faulting address. The M-mode handler
//   records mcause/mepc/mtval, advances mepc by 4 past the faulting
//   4-byte load, and resumes; the load's destination register keeps
//   a poison value, proving the load never completed.
// Phase C (control): MPRV=1, MPP=M. The same load must succeed and
//   return the canary, showing MPP=M preserves M-mode semantics.
// Phase D (restore): the PMP entry is rewritten and verified
//   unchanged, the load succeeds again with MPRV=0, then the PMP
//   entry is zeroed back to the boot state and the boot mstatus is
//   restored bit-for-bit.
//
// The probe loads live in inline asm under .option norvc so the
// faulting ld is exactly 4 bytes and the handler's fixed mepc+4
// skip lands on the csrc that clears MPRV. The load sites are
// global labels taken at run time, never hardcoded. Between the
// csrs that sets MPRV and the csrc that clears it, the only data
// memory access is the single probe ld: the compiler cannot see
// inside the volatile asm block, so nothing else can fault. The
// trap entry clears MPRV via mscratch before its first stack save
// (its own saves would otherwise fault as U-mode accesses).
//
// Machine model: QEMU 8.2.2 virt, single hart, M-mode throughout.
// medeleg and mideleg are zeroed so every trap lands in M-mode; mie
// and mstatus.MIE stay clear so no interrupt can pollute the trap
// counts.

#include "../uart.h"

extern void mprv_m_trap_entry(void);
extern char mprv_load_site_u;
extern char mprv_load_site_m;

#define MPRV_BIT (1UL << 17)
#define MPP_MASK (3UL << 11)

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U

static unsigned long mprv_page[512] __attribute__((aligned(4096)));
static const unsigned long CANARY = 0xC0FFEE1234567890UL;
static const unsigned long POISON = 0xBADC0DE0BADC0DE0UL;

volatile unsigned long mprv_m_traps;
static unsigned long mprv_cause[16];
static unsigned long mprv_epc[16];
static unsigned long mprv_tval[16];

static unsigned int checks = 0;
static unsigned int fails = 0;

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long rd_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static unsigned long rd_time(void) {
    unsigned long v;
    __asm__ volatile("rdtime %0" : "=r"(v));
    return v;
}

// M-mode trap handler. Records mcause/mepc/mtval. The one expected
// probe trap is a synchronous load access fault: advance mepc by 4
// to resume at the csrc that clears MPRV. Anything else is reported
// and parks the hart.
void mprv_m_handler(void) {
    unsigned long cause, epc, tval;
    __asm__ volatile("csrr %0, mcause" : "=r"(cause));
    __asm__ volatile("csrr %0, mepc" : "=r"(epc));
    __asm__ volatile("csrr %0, mtval" : "=r"(tval));
    if (mprv_m_traps < 16) {
        mprv_cause[mprv_m_traps] = cause;
        mprv_epc[mprv_m_traps] = epc;
        mprv_tval[mprv_m_traps] = tval;
    }
    mprv_m_traps++;
    if (cause == 0x5UL) {
        // Expected probe trap: skip the faulting 4-byte load.
        __asm__ volatile("csrw mepc, %0" :: "r"(epc + 4));
        return;
    }
    uart_puts("\nUNEXPECTED M-mode trap: mcause=");
    uart_put_hex(cause);
    uart_puts(" mepc=");
    uart_put_hex(epc);
    uart_puts(" mtval=");
    uart_put_hex(tval);
    uart_puts("\nRESULT: FAIL (unexpected M-mode trap)\n");
    for (;;)
        __asm__ volatile("wfi");
}

// Probe load with MPRV=1. The caller establishes MPP beforehand
// (U for phase B, M for phase C). Returns the loaded value, or the
// poison if the load trapped (the destination register is never
// written when the ld faults).
static unsigned long probe_load_u(unsigned long addr) {
    unsigned long v = POISON;
    __asm__ volatile(
        ".option norvc\n\t"
        "csrs mstatus, %1\n\t"   // MPRV=1; MPP as the caller set it
        ".global mprv_load_site_u\n"
        "mprv_load_site_u:\n\t"
        "ld %0, 0(%2)\n\t"       // probe load: traps iff MPP=U
        "csrc mstatus, %1\n\t"   // MPRV=0 (also reached via mepc+4)
        ".option rvc"
        : "+r"(v) : "r"(MPRV_BIT), "r"(addr) : "memory");
    return v;
}

static unsigned long probe_load_m(unsigned long addr) {
    unsigned long v = POISON;
    __asm__ volatile(
        ".option norvc\n\t"
        "csrs mstatus, %1\n\t"
        ".global mprv_load_site_m\n"
        "mprv_load_site_m:\n\t"
        "ld %0, 0(%2)\n\t"
        "csrc mstatus, %1\n\t"
        ".option rvc"
        : "+r"(v) : "r"(MPRV_BIT), "r"(addr) : "memory");
    return v;
}

static unsigned long fnv1a_64(unsigned long h, unsigned long v) {
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xffUL;
        h *= 0x100000001b3UL;
    }
    return h;
}

int main(void) {
    unsigned long mstatus_boot, pmpcfg0_boot, pmpaddr0_boot, pmpaddr1_boot;
    unsigned long page, pmpaddr0, pmpaddr1, pmpcfg0_rb;
    unsigned long site_u, site_m, v, traps_b, csum;
    unsigned long epc_ok, tval_ok, poison_ok;

    uart_init();
    uart_puts("mstatus-mprv-load: MPRV makes M-mode loads honor MPP\n");

    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)mprv_m_trap_entry));
    __asm__ volatile("csrw medeleg, %0" :: "r"(0UL));
    __asm__ volatile("csrw mideleg, %0" :: "r"(0UL));
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrci mstatus, 8");  // MIE off

    mstatus_boot = rd_mstatus();
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(pmpcfg0_boot));
    __asm__ volatile("csrr %0, pmpaddr0" : "=r"(pmpaddr0_boot));
    __asm__ volatile("csrr %0, pmpaddr1" : "=r"(pmpaddr1_boot));
    uart_puts("boot: mstatus=");
    uart_put_hex(mstatus_boot);
    uart_puts(" pmpcfg0=");
    uart_put_hex(pmpcfg0_boot);
    uart_puts("\n");
    check((mstatus_boot & MPRV_BIT) == 0, "MPRV set at boot");
    check(pmpcfg0_boot == 0, "PMP entry programmed at boot");
    check(pmpaddr0_boot == 0, "pmpaddr0 nonzero at boot");
    check(pmpaddr1_boot == 0, "pmpaddr1 nonzero at boot");

    // Write the canary before the PMP entry exists (M-mode store,
    // no PMP programmed yet).
    mprv_page[0] = CANARY;
    page = (unsigned long)&mprv_page[0];
    uart_puts("test page=");
    uart_put_hex(page);
    uart_puts("\n");

    // PMP: unlocked TOR entry over exactly the test page,
    // R=W=X=0. Unlocked entries do not apply to M-mode (so M-mode
    // loads succeed), but the region denies every lower-privilege
    // access (so a load checked as U-mode faults).
    pmpaddr0 = page >> 2;
    pmpaddr1 = (page + 4096) >> 2;
    __asm__ volatile("csrw pmpaddr0, %0" :: "r"(pmpaddr0));
    __asm__ volatile("csrw pmpaddr1, %0" :: "r"(pmpaddr1));
    __asm__ volatile("csrw pmpcfg0, %0" :: "r"(0x08UL));  // A=TOR, R=W=X=0, L=0
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(pmpcfg0_rb));
    {
        unsigned long a0, a1;
        __asm__ volatile("csrr %0, pmpaddr0" : "=r"(a0));
        __asm__ volatile("csrr %0, pmpaddr1" : "=r"(a1));
        uart_puts("pmp: pmpaddr0=");
        uart_put_hex(a0);
        uart_puts(" pmpaddr1=");
        uart_put_hex(a1);
        uart_puts(" pmpcfg0=");
        uart_put_hex(pmpcfg0_rb);
        uart_puts("\n");
        check(pmpcfg0_rb == 0x08UL, "pmpcfg0 readback != 0x08 (TOR deny-all)");
        check(a0 == pmpaddr0, "pmpaddr0 readback mismatch");
        check(a1 == pmpaddr1, "pmpaddr1 readback mismatch");
    }

    site_u = (unsigned long)&mprv_load_site_u;
    site_m = (unsigned long)&mprv_load_site_m;
    uart_puts("sites: load-u=");
    uart_put_hex(site_u);
    uart_puts(" load-m=");
    uart_put_hex(site_m);
    uart_puts("\n");

    // Phase A (control): MPRV=0, MPP=U. M-mode load bypasses the
    // unlocked PMP entry and must succeed.
    __asm__ volatile("csrc mstatus, %0" :: "r"(MPP_MASK));   // MPP=U
    __asm__ volatile("csrc mstatus, %0" :: "r"(MPRV_BIT));   // MPRV=0
    v = *(volatile unsigned long *)page;
    uart_puts("phaseA: value=");
    uart_put_hex(v);
    uart_puts(" traps=");
    uart_put_dec(mprv_m_traps);
    uart_puts("\n");
    check(mprv_m_traps == 0, "phase-A control load trapped");
    check(v == CANARY, "phase-A control load returned wrong value");

    // Phase B (probe): MPRV=1, MPP=U. The load is checked as a
    // U-mode access against the deny-all region and must fault.
    traps_b = mprv_m_traps;
    v = probe_load_u(page);
    uart_puts("phaseB: value=");
    uart_put_hex(v);
    uart_puts(" new-traps=");
    uart_put_dec(mprv_m_traps - traps_b);
    uart_puts(" cause=");
    uart_put_hex(mprv_cause[traps_b]);
    uart_puts(" mepc=");
    uart_put_hex(mprv_epc[traps_b]);
    uart_puts(" mtval=");
    uart_put_hex(mprv_tval[traps_b]);
    uart_puts("\n");
    check(mprv_m_traps - traps_b == 1, "phase-B probe did not trap exactly once");
    check(mprv_cause[traps_b] == 0x5UL, "phase-B trap was not a load access fault");
    epc_ok = (mprv_epc[traps_b] == site_u);
    check(epc_ok, "phase-B mepc != probe load site");
    tval_ok = (mprv_tval[traps_b] == page);
    check(tval_ok, "phase-B mtval != faulting address");
    poison_ok = (v == POISON);
    check(poison_ok, "phase-B load wrote its destination (fault did not suppress it)");

    // Phase C (control): MPRV=1, MPP=M. The load is checked as an
    // M-mode access, bypasses the unlocked entry, and must succeed.
    __asm__ volatile("csrs mstatus, %0" :: "r"(MPP_MASK));   // MPP=M
    traps_b = mprv_m_traps;
    v = probe_load_m(page);
    uart_puts("phaseC: value=");
    uart_put_hex(v);
    uart_puts(" new-traps=");
    uart_put_dec(mprv_m_traps - traps_b);
    uart_puts("\n");
    check(mprv_m_traps - traps_b == 0, "phase-C control load trapped");
    check(v == CANARY, "phase-C control load returned wrong value");

    // Phase D (restore): rewrite the PMP entry and verify it is
    // unchanged, confirm the load succeeds again with MPRV=0, then
    // zero the PMP entry back to the boot state and restore the
    // boot mstatus bit-for-bit.
    __asm__ volatile("csrw pmpaddr0, %0" :: "r"(pmpaddr0));
    __asm__ volatile("csrw pmpaddr1, %0" :: "r"(pmpaddr1));
    __asm__ volatile("csrw pmpcfg0, %0" :: "r"(0x08UL));
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(pmpcfg0_rb));
    check(pmpcfg0_rb == 0x08UL, "phase-D PMP entry did not survive the trap");
    __asm__ volatile("csrc mstatus, %0" :: "r"(MPP_MASK));   // MPP=U
    __asm__ volatile("csrc mstatus, %0" :: "r"(MPRV_BIT));   // MPRV=0
    v = *(volatile unsigned long *)page;
    uart_puts("phaseD: value=");
    uart_put_hex(v);
    uart_puts("\n");
    check(v == CANARY, "phase-D load after restore returned wrong value");
    __asm__ volatile("csrw pmpcfg0, %0" :: "r"(0UL));
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(pmpcfg0_rb));
    check(pmpcfg0_rb == 0, "pmpcfg0 not zeroed back to boot state");
    __asm__ volatile("csrw mstatus, %0" :: "r"(mstatus_boot));
    check(rd_mstatus() == mstatus_boot, "mstatus not restored bit-for-bit");

    csum = 0xcbf29ce484222325UL;
    csum = fnv1a_64(csum, (mstatus_boot >> 17) & 1UL);
    csum = fnv1a_64(csum, pmpcfg0_boot);
    csum = fnv1a_64(csum, 0x08UL);
    csum = fnv1a_64(csum, (v == CANARY) ? 1UL : 0UL);
    csum = fnv1a_64(csum, 1UL);                    // phase-B trap count
    csum = fnv1a_64(csum, mprv_cause[0]);
    csum = fnv1a_64(csum, epc_ok);
    csum = fnv1a_64(csum, tval_ok);
    csum = fnv1a_64(csum, poison_ok);
    csum = fnv1a_64(csum, 0UL);                    // phase-C new traps
    csum = fnv1a_64(csum, pmpcfg0_rb);
    csum = fnv1a_64(csum, (rd_mstatus() == mstatus_boot) ? 1UL : 0UL);
    csum = fnv1a_64(csum, checks);

    uart_puts("Checks: ");
    uart_put_dec(checks);
    uart_puts("\nMismatches: ");
    uart_put_dec(fails);
    uart_puts("\nChecksum: ");
    uart_put_hex(csum);
    uart_puts("\nEnvironment: QEMU 8.2.2\n");
    if (fails == 0)
        uart_puts("Verdict: PASS\n");
    else {
        uart_puts("Verdict: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }

    // Let the UART drain before touching the finisher device.
    {
        unsigned long drain = rd_time();
        while (rd_time() - drain < 100000UL)
            ;
    }

    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    // FAIL: park the hart without touching the finisher device. The
    // harness runs QEMU under timeout, so a FAIL is observable as
    // the timeout exit status (124) as well as the Verdict line.
    for (;;)
        __asm__ volatile("wfi");
}
