// pnm_main.c: PMP NAPOT match-behavior test.
//
// Exactly one mechanism is under test: how a NAPOT PMP entry
// matches an access address. pmpaddr0 = 0x200009ff encodes a 4 KiB
// naturally aligned region at [0x80002000, 0x80003000): the low 9
// bits are ones (9 trailing ones select 2^(9+3) = 4096 bytes) and
// the base decodes as ((0x200009ff & ~0x1ff) << 2) = 0x80002000.
// pmpcfg0 entry 0 = 0x18 means A = NAPOT (bits 4:3 = 0b11) with
// L = 0 and R = W = X = 0: an unlocked deny entry.
//
// An unlocked entry is not checked against M-mode accesses, so the
// probes run with mstatus.MPRV = 1 and MPP = S: each load/store is
// then privilege-checked as an S-mode access, which the unlocked
// entry does deny. (That MPRV makes M-mode data accesses honor MPP
// for PMP checks was verified on this QEMU by the
// mstatus-mprv-load module.)
//
// Two entries are programmed, because the privileged spec denies
// an S-mode access that matches NO entry whenever at least one
// entry is programmed. Entry 1 (pmpaddr1 = 0x200007ff, a 16 KiB
// NAPOT at 0x80000000, R = W = X = 1, unlocked) explicitly allows
// the outside probe addresses, so the only thing that can trap an
// outside probe is entry 0 matching it, which must not happen.
// Entry 0 has the lower index, so inside the deny region its
// no-permission verdict wins by the lowest-match-wins priority.
// An access inside the region must trap with the access-fault
// cause the spec assigns; an access one word outside the region
// does not match entry 0 at all, so entry 1 lets it complete with
// no trap.
//
// Probes (each a single volatile asm block so the layout is exact;
// .option norvc keeps every instruction 4 bytes, so the faulting
// access sits exactly at resume - 4):
//   T1a: lw at 0x80002000 (first word inside) traps once,
//        mcause = 0x5, mepc = faulting lw, mtval = 0x80002000,
//        destination keeps its poison value.
//   T1b: lw at 0x80002ffc (last word inside) traps once,
//        mcause = 0x5, mtval = 0x80002ffc, poison kept.
//   T2a: lw at 0x80001ffc (one word below the region) completes
//        with 0 traps and returns the canary.
//   T2b: lw at 0x80003000 (first word above the region) completes
//        with 0 traps and returns the canary.
//   T3a: sw at 0x80002000 traps once, mcause = 0x7,
//        mtval = 0x80002000.
//   T3b: sw at 0x80001ffc completes with 0 traps and round-trips.
//   T3c: sw at 0x80003000 completes with 0 traps and round-trips.
//
// A minimal M-mode trap entry (pnm_trap.S) records mcause/mepc/
// mtval, counts every entry, and resumes at the label the probe
// stored. A quiet window of ordinary M-mode work with the entry
// still programmed must show 0 new traps. Boot PMP values are
// restored exactly at the end, and the restore is checked.

#include "../uart.h"

extern void pnm_trap_entry(void);

// Trap save area, laid out for pnm_trap.S:
// [0]=total traps [1]=t1 [2]=mcause [3]=mepc [4]=mtval
// [5]=resume pc [6]=seen flag [7]=unused.
// mscratch points here while a test is armed.
volatile unsigned long pnm_save[8];

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final Verdict line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

#define MPRV_BIT (1UL << 17)
#define MPP_MASK (3UL << 11)
#define MPP_S    (1UL << 11)

#define PMPADDR0_VAL 0x200009ffUL  // 4 KiB NAPOT at 0x80002000
#define PMPADDR1_VAL 0x200007ffUL  // 16 KiB NAPOT at 0x80000000
#define PMPCFG0_VAL  0x1f18UL      // entry 1: NAPOT R=W=X=1 (allow),
                                   // entry 0: A=NAPOT, L=0, R=W=X=0 (deny)

#define ADDR_IN_LO 0x80002000UL  // first word inside the region
#define ADDR_IN_HI 0x80002ffcUL  // last word inside the region
#define ADDR_BELOW 0x80001ffcUL  // one word below the region
#define ADDR_ABOVE 0x80003000UL  // first word above the region

#define POISON_LD  0xdeadbeefdeadbeefUL
#define CANARY_IN  0x6c0de001UL
#define CANARY_IN2 0x6c0de002UL
#define CANARY_LO  0x6c0de0caUL
#define CANARY_HI  0x6c0deafeUL
#define STORE_VAL  0x5a5a5a5aUL

static unsigned long checks = 0;
static unsigned long fails = 0;
static unsigned long csum = 1469598103934665603UL;  // FNV-1a 64 offset basis

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// Fold one 64-bit value into the FNV-1a checksum.
static void fold(unsigned long v) {
    int i;
    for (i = 0; i < 8; i++) {
        csum ^= (v >> (i * 8)) & 0xffUL;
        csum *= 1099511628211UL;
    }
}

static unsigned long csr_read_pmpaddr0(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpaddr0" : "=r"(v));
    return v;
}

static void csr_write_pmpaddr0(unsigned long v) {
    __asm__ volatile("csrw pmpaddr0, %0" :: "r"(v));
}

static unsigned long csr_read_pmpaddr1(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpaddr1" : "=r"(v));
    return v;
}

static void csr_write_pmpaddr1(unsigned long v) {
    __asm__ volatile("csrw pmpaddr1, %0" :: "r"(v));
}

static unsigned long csr_read_pmpcfg0(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(v));
    return v;
}

static void csr_write_pmpcfg0(unsigned long v) {
    __asm__ volatile("csrw pmpcfg0, %0" :: "r"(v));
}

static unsigned long csr_read_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

// Probe load under MPRV=1/MPP=S. Returns the loaded word, or
// POISON_LD if the load trapped (a faulting load never writes its
// destination). The trap handler resumes at label 1; the faulting
// lw is the 4-byte instruction immediately before it, so mepc of a
// trapped probe must equal pnm_save[5] - 4.
static unsigned long probe_load(unsigned long addr) {
    unsigned long got;
    __asm__ volatile(
        ".option norvc\n\t"
        "la t0, 1f\n\t"
        "sd t0, 40(%1)\n\t"    // pnm_save[5]: resume pc
        "sd zero, 48(%1)\n\t"  // pnm_save[6]: seen = 0
        "csrc mstatus, %3\n\t" // MPP field = 0
        "csrs mstatus, %4\n\t" // MPP = S
        "csrs mstatus, %5\n\t" // MPRV = 1
        "mv t1, %2\n\t"        // poison the destination
        "lw t1, 0(%6)\n\t"     // probe load: faults iff addr matches
        "1:\n\t"
        "csrc mstatus, %5\n\t" // MPRV = 0 on both paths
        "mv %0, t1\n\t"        // capture the destination
        ".option rvc"
        : "=r"(got)
        : "r"(pnm_save), "r"(POISON_LD), "r"(MPP_MASK), "r"(MPP_S),
          "r"(MPRV_BIT), "r"(addr)
        : "t0", "t1", "memory");
    return got;
}

// Probe store under MPRV=1/MPP=S. Same resume-4 layout as
// probe_load; a faulting store traps before writing memory.
static void probe_store(unsigned long addr, unsigned long val) {
    __asm__ volatile(
        ".option norvc\n\t"
        "la t0, 1f\n\t"
        "sd t0, 40(%0)\n\t"    // pnm_save[5]: resume pc
        "sd zero, 48(%0)\n\t"  // pnm_save[6]: seen = 0
        "csrc mstatus, %2\n\t" // MPP field = 0
        "csrs mstatus, %3\n\t" // MPP = S
        "csrs mstatus, %4\n\t" // MPRV = 1
        "sw %1, 0(%5)\n\t"     // probe store: faults iff addr matches
        "1:\n\t"
        "csrc mstatus, %4\n\t" // MPRV = 0 on both paths
        ".option rvc"
        :
        : "r"(pnm_save), "r"(val), "r"(MPP_MASK), "r"(MPP_S),
          "r"(MPRV_BIT), "r"(addr)
        : "t0", "memory");
}

static unsigned long mem_load(unsigned long addr) {
    unsigned long v;
    __asm__ volatile("lw %0, 0(%1)" : "=r"(v) : "r"(addr));
    return v;
}

// 4-byte canary store: matches the probe width, so the four probe
// words never overlap each other.
static void mem_write32(unsigned long addr, unsigned long val) {
    *(volatile unsigned int *)addr = (unsigned int)val;
}

// Report one load probe: the raw trap record plus the verdict of
// each check. seen_exp is 1 for inside probes, 0 for outside.
static void report_load(const char *tag, unsigned long addr,
                        unsigned long expect_val, int seen_exp,
                        unsigned long cause_exp) {
    unsigned long got = probe_load(addr);  // run the probe first; the
                                           // trap record below is its own
    unsigned long seen = pnm_save[6];
    unsigned long cause = pnm_save[2];
    unsigned long epc = pnm_save[3];
    unsigned long tval = pnm_save[4];
    unsigned long resume = pnm_save[5];

    uart_puts(tag);
    uart_puts(": seen=");
    uart_put_dec(seen);
    uart_puts(" mcause=");
    uart_put_hex(cause);
    uart_puts(" mepc=");
    uart_put_hex(epc);
    uart_puts(" mtval=");
    uart_put_hex(tval);
    uart_puts(" resume-4=");
    uart_put_hex(resume - 4);
    uart_puts(" val=");
    uart_put_hex(got);
    uart_puts("\n");

    check(seen == (unsigned long)seen_exp, "load probe trap count wrong");
    fold(seen);
    if (seen_exp) {
        check(cause == cause_exp, "load probe mcause wrong");
        check(epc == resume - 4, "load probe mepc != faulting lw");
        check(tval == addr, "load probe mtval != faulting address");
        check(got == POISON_LD, "load probe overwrote its destination");
        fold(cause);
        fold(epc == resume - 4);
        fold(tval == addr);
        fold(got == POISON_LD);
    } else {
        check(got == expect_val, "load probe outside region wrong value");
        fold(got == expect_val);
    }
}

// Report one store probe. For outside probes the store must also
// round-trip through a plain M-mode load.
static void report_store(const char *tag, unsigned long addr,
                         int seen_exp, unsigned long cause_exp) {
    unsigned long seen, cause, epc, tval, resume, rb;

    probe_store(addr, STORE_VAL);
    seen = pnm_save[6];
    cause = pnm_save[2];
    epc = pnm_save[3];
    tval = pnm_save[4];
    resume = pnm_save[5];

    uart_puts(tag);
    uart_puts(": seen=");
    uart_put_dec(seen);
    uart_puts(" mcause=");
    uart_put_hex(cause);
    uart_puts(" mepc=");
    uart_put_hex(epc);
    uart_puts(" mtval=");
    uart_put_hex(tval);
    uart_puts(" resume-4=");
    uart_put_hex(resume - 4);
    uart_puts("\n");

    check(seen == (unsigned long)seen_exp, "store probe trap count wrong");
    fold(seen);
    if (seen_exp) {
        check(cause == cause_exp, "store probe mcause wrong");
        check(epc == resume - 4, "store probe mepc != faulting sw");
        check(tval == addr, "store probe mtval != faulting address");
        fold(cause);
        fold(epc == resume - 4);
        fold(tval == addr);
    } else {
        rb = mem_load(addr);
        uart_puts("  roundtrip=");
        uart_put_hex(rb);
        uart_puts("\n");
        check(rb == STORE_VAL, "store probe outside region did not round-trip");
        fold(rb == STORE_VAL);
    }
}

int main(void) {
    unsigned long boot_addr0, boot_addr1, boot_cfg0, boot_mstatus;
    unsigned long rb, q0, acc;
    int i;
    volatile unsigned long acc_v = 0;

    uart_init();
    uart_puts("pmp-napot-match: PMP NAPOT match-behavior test\n");

    // All traps to M-mode, no interrupts: the only traps possible
    // are the probes' own synchronous faults.
    __asm__ volatile("csrw mtvec, %0" :: "r"(pnm_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(pnm_save));
    __asm__ volatile("csrw medeleg, %0" :: "r"(0UL));
    __asm__ volatile("csrw mideleg, %0" :: "r"(0UL));
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrci mstatus, 8");  // MIE off

    boot_addr0 = csr_read_pmpaddr0();
    boot_addr1 = csr_read_pmpaddr1();
    boot_cfg0 = csr_read_pmpcfg0();
    boot_mstatus = csr_read_mstatus();
    uart_puts("boot: pmpaddr0=");
    uart_put_hex(boot_addr0);
    uart_puts(" pmpaddr1=");
    uart_put_hex(boot_addr1);
    uart_puts(" pmpcfg0=");
    uart_put_hex(boot_cfg0);
    uart_puts(" mstatus=");
    uart_put_hex(boot_mstatus);
    uart_puts("\n");
    check(boot_addr0 == 0, "pmpaddr0 nonzero at boot");
    check(boot_addr1 == 0, "pmpaddr1 nonzero at boot");
    check(boot_cfg0 == 0, "pmpcfg0 nonzero at boot");
    fold(boot_addr0);
    fold(boot_addr1);
    fold(boot_cfg0);
    fold(boot_mstatus);

    // Layout guard, two parts. (a) The trap handler's first store
    // lands in pnm_save, which must sit outside the deny region:
    // pnm_save lives in .bss, and the check requires the whole
    // 8-word array below 0x80002000. (On trap entry the hardware
    // sets MPP to the previous mode, M, so with MPRV still set the
    // handler's stores are privilege-checked as M-mode and the
    // unlocked entries never apply to them; the address check here
    // is belt and braces.) (b) The 16 KiB boot stack reservation overlaps the scratch page, so
    // the probe and canary addresses are only safe if the LIVE
    // stack never reaches them. The worst-case stack depth here is
    // main(112) + report_load(96) + check(16) = 224 bytes below
    // _stack_top (frame sizes from the disassembly; the trap
    // handler uses no C stack and interrupts stay off), so
    // requiring sp 1 KiB clear of the region top leaves 4x margin.
    // Both values are run-time constants, identical every run.
    {
        unsigned long sp_now;
        __asm__ volatile("mv %0, sp" : "=r"(sp_now));
        uart_puts("layout: pnm_save=");
        uart_put_hex((unsigned long)pnm_save);
        uart_puts(" sp=");
        uart_put_hex(sp_now);
        uart_puts("\n");
        check((unsigned long)pnm_save + sizeof(pnm_save) <= 0x80002000UL,
              "pnm_save reaches the PMP region");
        fold((unsigned long)pnm_save + sizeof(pnm_save) <= 0x80002000UL);
        check(sp_now > 0x80003000UL + 0x400UL,
              "live stack within 1 KiB of the scratch region");
        fold(sp_now > 0x80003400UL);
    }

    // Control: all four probe addresses are good RAM before any PMP
    // programming. Written in M-mode with no entry programmed;
    // 4-byte stores match the probe width so no word overlaps its
    // neighbor.
    mem_write32(ADDR_IN_LO, CANARY_IN);
    mem_write32(ADDR_IN_HI, CANARY_IN2);
    mem_write32(ADDR_BELOW, CANARY_LO);
    mem_write32(ADDR_ABOVE, CANARY_HI);
    check(mem_load(ADDR_IN_LO) == CANARY_IN,
          "scratch word 0 not writable before PMP");
    check(mem_load(ADDR_IN_HI) == CANARY_IN2,
          "scratch last word not writable before PMP");
    check(mem_load(ADDR_BELOW) == CANARY_LO,
          "word below region not writable before PMP");
    check(mem_load(ADDR_ABOVE) == CANARY_HI,
          "word above region not writable before PMP");
    uart_puts("control: probe addresses are good RAM: ok\n");

    // Program the entries. Entry 0: unlocked NAPOT 4 KiB deny over
    // the scratch page. Entry 1: unlocked 16 KiB NAPOT allow over
    // [0x80000000, 0x80004000), so S-mode accesses outside entry 0
    // complete instead of hitting the spec's default-deny for
    // unmatched S-mode accesses. pmpcfg0 = 0x1f18: entry 1 is
    // 0x1f (NAPOT, R = W = X = 1), entry 0 is 0x18 (NAPOT, no
    // permissions).
    csr_write_pmpaddr0(PMPADDR0_VAL);
    csr_write_pmpaddr1(PMPADDR1_VAL);
    csr_write_pmpcfg0(PMPCFG0_VAL);
    rb = csr_read_pmpaddr0();
    uart_puts("config: pmpaddr0=");
    uart_put_hex(rb);
    uart_puts(" (expect 0x200009ff)\n");
    check(rb == PMPADDR0_VAL, "pmpaddr0 readback != 0x200009ff");
    fold(rb);
    rb = csr_read_pmpaddr1();
    uart_puts("config: pmpaddr1=");
    uart_put_hex(rb);
    uart_puts(" (expect 0x200007ff)\n");
    check(rb == PMPADDR1_VAL, "pmpaddr1 readback != 0x200007ff");
    fold(rb);
    rb = csr_read_pmpcfg0();
    uart_puts("config: pmpcfg0=");
    uart_put_hex(rb);
    uart_puts(" (expect 0x1f18)\n");
    check(rb == PMPCFG0_VAL, "pmpcfg0 readback != 0x1f18");
    fold(rb);

    // T1a/T1b: loads inside the region trap exactly once each.
    report_load("t1a load in-lo ", ADDR_IN_LO, 0, 1, 0x5UL);
    report_load("t1b load in-hi ", ADDR_IN_HI, 0, 1, 0x5UL);
    // T2a/T2b: loads one word outside the region complete, no trap.
    report_load("t2a load below ", ADDR_BELOW, CANARY_LO, 0, 0);
    report_load("t2b load above ", ADDR_ABOVE, CANARY_HI, 0, 0);
    // T3a: store inside traps once with the store fault cause.
    report_store("t3a store in-lo", ADDR_IN_LO, 1, 0x7UL);
    // T3b/T3c: stores one word outside complete and round-trip.
    report_store("t3b store below", ADDR_BELOW, 0, 0);
    report_store("t3c store above", ADDR_ABOVE, 0, 0);

    // Quiet window: ordinary M-mode work with the entry still
    // programmed and no probe armed. Any trap here would be
    // unexpected; the handler counts every entry in pnm_save[0].
    q0 = pnm_save[0];
    for (i = 0; i < 10000; i++) {
        unsigned long ms;
        __asm__ volatile("csrr %0, mstatus" : "=r"(ms));
        acc_v ^= ms ^ (unsigned long)i;
    }
    acc = acc_v;
    uart_puts("quiet: traps before=");
    uart_put_dec(q0);
    uart_puts(" after=");
    uart_put_dec(pnm_save[0]);
    uart_puts("\n");
    check(pnm_save[0] == q0, "quiet window saw an unexpected trap");
    fold(pnm_save[0] - q0);
    fold(acc);  // deterministic: mstatus and the loop index only

    // Restore boot PMP values exactly, and check the restore.
    csr_write_pmpaddr0(boot_addr0);
    csr_write_pmpaddr1(boot_addr1);
    csr_write_pmpcfg0(boot_cfg0);
    rb = csr_read_pmpaddr0();
    uart_puts("restore: pmpaddr0=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == boot_addr0, "pmpaddr0 restore != boot value");
    fold(rb);
    rb = csr_read_pmpaddr1();
    uart_puts("restore: pmpaddr1=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == boot_addr1, "pmpaddr1 restore != boot value");
    fold(rb);
    rb = csr_read_pmpcfg0();
    uart_puts("restore: pmpcfg0=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == boot_cfg0, "pmpcfg0 restore != boot value");
    fold(rb);
    __asm__ volatile("csrw mstatus, %0" :: "r"(boot_mstatus));
    check(csr_read_mstatus() == boot_mstatus, "mstatus not restored");
    fold(csr_read_mstatus() == boot_mstatus);

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
        uart_put_dec(fails);
        uart_puts(" checks failed)\n");
    }

    while (!(*UART0_LSR & LSR_TEMT))
        ;
    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    // FAIL: park the hart without touching the finisher device.
    for (;;)
        __asm__ volatile("wfi");
}
