// tvt_main.c: the M-mode virtual-memory trap switch mstatus.TVM
// (backlog item "riscv mstatus-tvm-trap").
//
// The behavior under test: bit 20 of mstatus, TVM (Trap Virtual
// Memory), is an M-mode-only switch. When TVM=1, S-mode accesses to
// the satp CSR (reads AND writes) and S-mode sfence.vma each raise
// an illegal-instruction trap; when TVM=0 the same operations
// execute normally. The privileged spec (section 3.1.6) gives TVM to
// M-mode so a hypervisor-style monitor keeps exclusive control of
// virtual memory; sstatus has no TVM bit, so S-mode cannot observe
// or change the switch, only feel its effect.
//
// Premise correction, measured on the hart: the backlog gloss said
// "writes to satp trap", but the first QEMU run showed the S-mode
// satp READ trapping as well (mcause=0x2, mepc at the csrr site).
// The spec text covers "attempts to read or write the satp CSR", so
// the module proves the true rule: with TVM=1 the probe pad takes
// three illegal-instruction traps (satp write site, satp read site,
// sfence.vma site), and with TVM=0 it takes none.
//
// Siblings: mstatus-tw-trap tests the TW (timeout-wait) trap bit, a
// different mstatus switch with a different trapped operation;
// satp-mode-warl probes the satp MODE field's WARL legalization but
// never traps on the access itself. This module tests the access
// trap: the operation is legal in S-mode with the switch clear and
// illegal with it set.
//
// Machine model: QEMU 8.2.2 virt, single hart, M-mode throughout
// except the two S-mode probe excursions. medeleg and mideleg stay
// zero so every trap lands in M-mode; mie and mstatus.MIE stay clear
// so no interrupt can pollute the trap counts. A PMP NAPOT entry
// opens the whole address space to S-mode (with no PMP entry,
// lower-privilege fetches fault).
//
// Phase 1 (TVM set): M-mode sets mstatus.TVM and sret to the S-mode
// probe pad (tvt_smode_pad in tvt_trap.S). The pad writes 0 (the Bare
// MODE value, so translation is never enabled) to satp, reads satp
// back, executes sfence.vma, then ecalls back to M-mode. Require:
// exactly 3 probe traps, all mcause=0x2, mepc exactly at the satp
// write site, the satp read site, and the fence site respectively;
// the trapped satp write had no effect (satp still 0 when read from
// M-mode); then the phase-end ecall (mcause=0x9) with mepc inside
// the pad range.
// Phase 2 (TVM clear): M-mode clears TVM and repeats the pad.
// Require: 0 probe traps, the phase-end ecall with mepc inside the
// pad range, satp readback 0 (the written value stuck).
// Finally the boot mstatus is restored bit-for-bit.
//
// All probe instructions are emitted under .option norvc (4 bytes
// each), so the handler's fixed mepc+4 skip always resumes at the
// next probe instruction. Expected site addresses come from in-asm
// global labels taken at run time, never hardcoded. A 64-bit FNV-1a
// checksum over the deterministic measured values is printed and
// cross-checked in the PROOF.md results table.

#include "../uart.h"

extern void tvt_m_trap_entry(void);
extern void tvt_phase1(void);
extern void tvt_phase2(void);
extern char tvt_satp_site;
extern char tvt_satp_read_site;
extern char tvt_fence_site;
extern char tvt_pad_lo;
extern char tvt_pad_hi;

volatile unsigned long tvt_resume_pc;
volatile unsigned int tvt_expect_ecall;
volatile unsigned long tvt_satp_rb;

volatile unsigned long tvt_m_traps;
volatile unsigned long tvt_phase;   // 1 or 2 while a phase runs
static unsigned long tvt_cause[8];
static unsigned long tvt_epc[8];
static unsigned long tvt_tval[8];

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

static unsigned long rd_satp(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, satp" : "=r"(v));
    return v;
}

// M-mode trap handler. Records every trap (cause/epc/tval). The two
// expected phase-1 probe traps are synchronous illegal-instruction
// faults: advance mepc by 4 to resume at the next probe instruction.
// The phase-end S-mode ecall redirects mepc at the resume label the
// phase driver registered and raises MPP to M-mode, so mret resumes
// the M-mode driver (mret returns to MPP, which the ecall trap set to
// S-mode; without the MPP fixup we would mret back into S-mode and
// the next M-mode CSR access in main would fault). Anything else is
// reported and parks the hart.
void tvt_m_handler(void) {
    unsigned long cause, epc, tval;
    __asm__ volatile("csrr %0, mcause" : "=r"(cause));
    __asm__ volatile("csrr %0, mepc" : "=r"(epc));
    __asm__ volatile("csrr %0, mtval" : "=r"(tval));
    if (tvt_m_traps < 8) {
        tvt_cause[tvt_m_traps] = cause;
        tvt_epc[tvt_m_traps] = epc;
        tvt_tval[tvt_m_traps] = tval;
    }
    tvt_m_traps++;
    if (cause == 0x2UL && tvt_phase == 1) {
        // Expected probe trap: skip the faulting 4-byte instruction.
        __asm__ volatile("csrw mepc, %0" :: "r"(epc + 4));
        return;
    }
    if (cause == 0x9UL && tvt_expect_ecall != 0) {
        unsigned long ms;
        __asm__ volatile("csrr %0, mstatus" : "=r"(ms));
        ms = (ms & ~(3UL << 11)) | (3UL << 11);  // MPP = M-mode
        __asm__ volatile("csrw mstatus, %0" :: "r"(ms));
        __asm__ volatile("csrw mepc, %0" :: "r"(tvt_resume_pc));
        tvt_expect_ecall = 0;
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

static unsigned long fnv1a_64(unsigned long h, unsigned long v) {
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xffUL;
        h *= 0x100000001b3UL;
    }
    return h;
}

#define TVM_BIT (1UL << 20)

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U

int main(void) {
    unsigned long mstatus_boot, mstatus_tvm_set, mstatus_tvm_clear;
    unsigned long satp_site, satp_read_site, fence_site, pad_lo, pad_hi;
    unsigned long traps_p1, traps_p2, satp_rb_p2, csum;
    unsigned long epc1_ok, epc2_ok, epc3_ok, ecall_epc_ok;

    uart_init();
    uart_puts("mstatus-tvm-trap: M-mode virtual-memory trap switch mstatus.TVM\n");

    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)tvt_m_trap_entry));

    // PMP: open the whole address space to S-mode (no entry means
    // lower-privilege accesses fault).
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    mstatus_boot = rd_mstatus();
    uart_puts("boot: mstatus=");
    uart_put_hex(mstatus_boot);
    uart_puts("\n");
    check((mstatus_boot & TVM_BIT) == 0, "TVM set at boot");

    // Disarm the M-mode interrupt path: mie clear, mstatus.MIE clear.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrci mstatus, 8");  // MIE off

    satp_site = (unsigned long)&tvt_satp_site;
    satp_read_site = (unsigned long)&tvt_satp_read_site;
    fence_site = (unsigned long)&tvt_fence_site;
    pad_lo = (unsigned long)&tvt_pad_lo;
    pad_hi = (unsigned long)&tvt_pad_hi;
    uart_puts("pad: satp-site=");
    uart_put_hex(satp_site);
    uart_puts(" satp-read-site=");
    uart_put_hex(satp_read_site);
    uart_puts(" fence-site=");
    uart_put_hex(fence_site);
    uart_puts("\n");

    // Phase 1: M-mode sets the TVM switch.
    __asm__ volatile("csrs mstatus, %0" :: "r"(TVM_BIT));
    mstatus_tvm_set = rd_mstatus();
    uart_puts("phase1: mstatus-after-set=");
    uart_put_hex(mstatus_tvm_set);
    uart_puts("\n");
    check((mstatus_tvm_set & TVM_BIT) != 0, "TVM did not set");
    check((mstatus_tvm_set & ~TVM_BIT) == (mstatus_boot & ~TVM_BIT),
          "setting TVM disturbed another mstatus bit");

    tvt_phase = 1;
    tvt_phase1();
    tvt_phase = 0;
    traps_p1 = tvt_m_traps;

    uart_puts("phase1: traps=");
    uart_put_dec(traps_p1);
    uart_puts(" cause0=");
    uart_put_hex(tvt_cause[0]);
    uart_puts(" epc0=");
    uart_put_hex(tvt_epc[0]);
    uart_puts(" cause1=");
    uart_put_hex(tvt_cause[1]);
    uart_puts(" epc1=");
    uart_put_hex(tvt_epc[1]);
    uart_puts(" cause2=");
    uart_put_hex(tvt_cause[2]);
    uart_puts(" epc2=");
    uart_put_hex(tvt_epc[2]);
    uart_puts(" cause3=");
    uart_put_hex(tvt_cause[3]);
    uart_puts(" epc3=");
    uart_put_hex(tvt_epc[3]);
    uart_puts(" satp-now=");
    uart_put_hex(rd_satp());
    uart_puts("\n");

    check(traps_p1 == 4, "phase-1 trap count != 4 (3 probes + ecall)");
    check(tvt_cause[0] == 0x2UL, "phase-1 first trap was not illegal-instruction");
    epc1_ok = (tvt_epc[0] == satp_site);
    check(epc1_ok, "phase-1 first trap mepc != satp write site");
    check(tvt_cause[1] == 0x2UL, "phase-1 second trap was not illegal-instruction");
    epc2_ok = (tvt_epc[1] == satp_read_site);
    check(epc2_ok, "phase-1 second trap mepc != satp read site");
    check(tvt_cause[2] == 0x2UL, "phase-1 third trap was not illegal-instruction");
    epc3_ok = (tvt_epc[2] == fence_site);
    check(epc3_ok, "phase-1 third trap mepc != sfence.vma site");
    check(tvt_cause[3] == 0x9UL, "phase-1 fourth trap was not the S-mode ecall");
    ecall_epc_ok = (tvt_epc[3] >= pad_lo && tvt_epc[3] <= pad_hi);
    check(ecall_epc_ok, "phase-1 ecall mepc outside the pad range");
    check(rd_satp() == 0, "trapped satp write had a visible effect");

    // Phase 2: M-mode clears the TVM switch; the same pad must now
    // run clean.
    __asm__ volatile("csrc mstatus, %0" :: "r"(TVM_BIT));
    mstatus_tvm_clear = rd_mstatus();
    uart_puts("phase2: mstatus-after-clear=");
    uart_put_hex(mstatus_tvm_clear);
    uart_puts("\n");
    check((mstatus_tvm_clear & TVM_BIT) == 0, "TVM did not clear");

    tvt_phase = 2;
    tvt_phase2();
    tvt_phase = 0;
    traps_p2 = tvt_m_traps - traps_p1;
    satp_rb_p2 = tvt_satp_rb;

    uart_puts("phase2: new-traps=");
    uart_put_dec(traps_p2);
    uart_puts(" ecall-cause=");
    uart_put_hex(tvt_cause[traps_p1]);
    uart_puts(" ecall-epc=");
    uart_put_hex(tvt_epc[traps_p1]);
    uart_puts(" satp-rb=");
    uart_put_hex(satp_rb_p2);
    uart_puts("\n");

    check(traps_p2 == 1, "phase-2 trap count != 1 (ecall only)");
    check(tvt_cause[traps_p1] == 0x9UL, "phase-2 trap was not the S-mode ecall");
    check(tvt_epc[traps_p1] >= pad_lo && tvt_epc[traps_p1] <= pad_hi,
          "phase-2 ecall mepc outside the pad range");
    check(satp_rb_p2 == 0, "satp write of Bare value did not read back");

    // Restore the boot mstatus bit-for-bit.
    __asm__ volatile("csrw mstatus, %0" :: "r"(mstatus_boot));
    check(rd_mstatus() == mstatus_boot, "mstatus not restored bit-for-bit");

    csum = 0xcbf29ce484222325UL;
    csum = fnv1a_64(csum, (mstatus_tvm_set >> 20) & 1UL);
    csum = fnv1a_64(csum, traps_p1);
    csum = fnv1a_64(csum, tvt_cause[0]);
    csum = fnv1a_64(csum, epc1_ok);
    csum = fnv1a_64(csum, tvt_cause[1]);
    csum = fnv1a_64(csum, epc2_ok);
    csum = fnv1a_64(csum, tvt_cause[2]);
    csum = fnv1a_64(csum, epc3_ok);
    csum = fnv1a_64(csum, tvt_cause[3]);
    csum = fnv1a_64(csum, ecall_epc_ok);
    csum = fnv1a_64(csum, (mstatus_tvm_clear >> 20) & 1UL);
    csum = fnv1a_64(csum, traps_p2);
    csum = fnv1a_64(csum, tvt_cause[traps_p1]);
    csum = fnv1a_64(csum, satp_rb_p2);
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
