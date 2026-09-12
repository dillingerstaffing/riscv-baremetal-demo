// tsr_main.c: the M-mode sret trap switch mstatus.TSR
// (backlog item "riscv mstatus-tsr-trap").
//
// The behavior under test: bit 22 of mstatus, TSR (Trap SRET), is an
// M-mode-only switch. When TSR=1, an sret executed in S-mode raises
// an illegal-instruction trap; when TSR=0 the same sret returns to
// S-mode normally. The privileged spec (section 3.1.6) gives TSR to
// M-mode so a monitor keeps S-mode from returning to itself;
// sstatus has no TSR bit, so S-mode cannot observe or change the
// switch, only feel its effect.
//
// Siblings: mstatus-tvm-trap tests the TVM (virtual-memory) trap
// bit, a different mstatus switch trapping different operations
// (satp access, sfence.vma); mstatus-tw-trap tests the TW
// (timeout-wait) bit. This module tests the sret gate: the
// operation is legal in S-mode with the switch clear and illegal
// with it set.
//
// Machine model: QEMU 8.2.2 virt, single hart, M-mode throughout
// except the two S-mode probe excursions. medeleg and mideleg stay
// zero so every trap lands in M-mode; mie and mstatus.MIE stay clear
// so no interrupt can pollute the trap counts. A PMP NAPOT entry
// opens the whole address space to S-mode (with no PMP entry,
// lower-privilege fetches fault).
//
// Phase 1 (TSR clear): M-mode verifies TSR reads 0 and srets to the
// S-mode probe pad (tsr_smode_pad in tsr_trap.S). The pad re-arms
// SPP=1 and sepc to the post-sret point, executes the probe sret,
// then ecalls back to M-mode. Require: exactly 1 trap total (the
// phase-end ecall, mcause=0x9, mepc exactly at the ecall site),
// which proves the sret returned normally with 0 M-mode traps.
// Phase 2 (TSR set): M-mode sets TSR (reads back to confirm the bit
// is 1) and repeats the pad. Require: exactly 2 traps, the first an
// illegal-instruction trap (mcause=0x2) with mepc exactly at the
// sret site, the second the phase-end ecall (mcause=0x9, mepc at the
// ecall site). Then TSR is cleared and the boot mstatus restored
// bit-for-bit.
//
// The probe sret and the following ecall are emitted under
// .option norvc (4 bytes each), so the handler's fixed mepc+4 skip
// past the faulting sret always resumes at the ecall. Expected site
// addresses come from in-asm global labels taken at run time, never
// hardcoded. A 64-bit FNV-1a checksum over the deterministic
// measured values is printed and cross-checked in the PROOF.md
// results table.

#include "../uart.h"

extern void tsr_m_trap_entry(void);
extern void tsr_phase1(void);
extern void tsr_phase2(void);
extern char tsr_sret_site;
extern char tsr_ecall_site;
extern char tsr_pad_lo;
extern char tsr_pad_hi;

volatile unsigned long tsr_resume_pc;
volatile unsigned int tsr_expect_ecall;

volatile unsigned long tsr_m_traps;
volatile unsigned long tsr_phase;   // 1 or 2 while a phase runs
static unsigned long tsr_cause[8];
static unsigned long tsr_epc[8];
static unsigned long tsr_tval[8];

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

// M-mode trap handler. Records every trap (cause/epc/mtval). The
// expected phase-2 sret trap is a synchronous illegal-instruction
// fault: advance mepc by 4 to resume at the ecall. The phase-end
// S-mode ecall redirects mepc at the resume label the phase driver
// registered and raises MPP to M-mode, so mret resumes the M-mode
// driver (mret returns to MPP, which the ecall trap set to S-mode;
// without the MPP fixup we would mret back into S-mode and the next
// M-mode CSR access in main would fault). Anything else is reported
// and parks the hart.
void tsr_m_handler(void) {
    unsigned long cause, epc, tval;
    __asm__ volatile("csrr %0, mcause" : "=r"(cause));
    __asm__ volatile("csrr %0, mepc" : "=r"(epc));
    __asm__ volatile("csrr %0, mtval" : "=r"(tval));
    if (tsr_m_traps < 8) {
        tsr_cause[tsr_m_traps] = cause;
        tsr_epc[tsr_m_traps] = epc;
        tsr_tval[tsr_m_traps] = tval;
    }
    tsr_m_traps++;
    if (cause == 0x2UL && tsr_phase == 2) {
        // Expected probe trap: skip the faulting 4-byte sret.
        __asm__ volatile("csrw mepc, %0" :: "r"(epc + 4));
        return;
    }
    if (cause == 0x9UL && tsr_expect_ecall != 0) {
        unsigned long ms;
        __asm__ volatile("csrr %0, mstatus" : "=r"(ms));
        ms = (ms & ~(3UL << 11)) | (3UL << 11);  // MPP = M-mode
        __asm__ volatile("csrw mstatus, %0" :: "r"(ms));
        __asm__ volatile("csrw mepc, %0" :: "r"(tsr_resume_pc));
        tsr_expect_ecall = 0;
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

#define TSR_BIT (1UL << 22)

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U

int main(void) {
    unsigned long mstatus_boot, mstatus_pre_set, mstatus_tsr_set, mstatus_tsr_clear;
    unsigned long sret_site, ecall_site, pad_lo, pad_hi;
    unsigned long traps_p1, traps_p2, csum;
    unsigned long ecall_epc_ok_p1, epc0_ok_p2, ecall_epc_ok_p2;

    uart_init();
    uart_puts("mstatus-tsr-trap: M-mode sret trap switch mstatus.TSR\n");

    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)tsr_m_trap_entry));

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
    check((mstatus_boot & TSR_BIT) == 0, "TSR set at boot");

    // Disarm the M-mode interrupt path: mie clear, mstatus.MIE clear.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrci mstatus, 8");  // MIE off

    sret_site = (unsigned long)&tsr_sret_site;
    ecall_site = (unsigned long)&tsr_ecall_site;
    pad_lo = (unsigned long)&tsr_pad_lo;
    pad_hi = (unsigned long)&tsr_pad_hi;
    uart_puts("pad: sret-site=");
    uart_put_hex(sret_site);
    uart_puts(" ecall-site=");
    uart_put_hex(ecall_site);
    uart_puts("\n");

    // Phase 1: TSR clear (boot state). The pad's sret must return to
    // S-mode with no M-mode trap; the only trap is the phase-end
    // ecall.
    tsr_phase = 1;
    tsr_phase1();
    tsr_phase = 0;
    traps_p1 = tsr_m_traps;

    uart_puts("phase1: traps=");
    uart_put_dec(traps_p1);
    uart_puts(" ecall-cause=");
    uart_put_hex(tsr_cause[0]);
    uart_puts(" ecall-epc=");
    uart_put_hex(tsr_epc[0]);
    uart_puts("\n");

    check(traps_p1 == 1, "phase-1 trap count != 1 (ecall only)");
    check(tsr_cause[0] == 0x9UL, "phase-1 trap was not the S-mode ecall");
    ecall_epc_ok_p1 = (tsr_epc[0] == ecall_site);
    check(ecall_epc_ok_p1, "phase-1 ecall mepc != ecall site");
    check(tsr_epc[0] >= pad_lo && tsr_epc[0] <= pad_hi,
          "phase-1 ecall mepc outside the pad range");

    // Phase 2: M-mode sets the TSR switch; the S-mode sret must now
    // trap. The pre-set readback is taken after phase 1, because
    // phase 1's S-mode excursion legitimately moved SIE/SPIE/MPIE.
    mstatus_pre_set = rd_mstatus();
    __asm__ volatile("csrs mstatus, %0" :: "r"(TSR_BIT));
    mstatus_tsr_set = rd_mstatus();
    uart_puts("phase2: mstatus-before-set=");
    uart_put_hex(mstatus_pre_set);
    uart_puts(" mstatus-after-set=");
    uart_put_hex(mstatus_tsr_set);
    uart_puts("\n");
    check((mstatus_tsr_set & TSR_BIT) != 0, "TSR did not set");
    check((mstatus_tsr_set & ~TSR_BIT) == (mstatus_pre_set & ~TSR_BIT),
          "setting TSR disturbed another mstatus bit");

    tsr_phase = 2;
    tsr_phase2();
    tsr_phase = 0;
    traps_p2 = tsr_m_traps - traps_p1;

    uart_puts("phase2: new-traps=");
    uart_put_dec(traps_p2);
    uart_puts(" cause0=");
    uart_put_hex(tsr_cause[traps_p1]);
    uart_puts(" epc0=");
    uart_put_hex(tsr_epc[traps_p1]);
    uart_puts(" cause1=");
    uart_put_hex(tsr_cause[traps_p1 + 1]);
    uart_puts(" epc1=");
    uart_put_hex(tsr_epc[traps_p1 + 1]);
    uart_puts("\n");

    check(traps_p2 == 2, "phase-2 trap count != 2 (sret trap + ecall)");
    check(tsr_cause[traps_p1] == 0x2UL,
          "phase-2 first trap was not illegal-instruction");
    epc0_ok_p2 = (tsr_epc[traps_p1] == sret_site);
    check(epc0_ok_p2, "phase-2 first trap mepc != sret site");
    check(tsr_cause[traps_p1 + 1] == 0x9UL,
          "phase-2 second trap was not the S-mode ecall");
    ecall_epc_ok_p2 = (tsr_epc[traps_p1 + 1] == ecall_site);
    check(ecall_epc_ok_p2, "phase-2 ecall mepc != ecall site");

    // Clear TSR and restore the boot mstatus bit-for-bit.
    __asm__ volatile("csrc mstatus, %0" :: "r"(TSR_BIT));
    mstatus_tsr_clear = rd_mstatus();
    check((mstatus_tsr_clear & TSR_BIT) == 0, "TSR did not clear");
    __asm__ volatile("csrw mstatus, %0" :: "r"(mstatus_boot));
    check(rd_mstatus() == mstatus_boot, "mstatus not restored bit-for-bit");

    csum = 0xcbf29ce484222325UL;
    csum = fnv1a_64(csum, (mstatus_boot >> 22) & 1UL);
    csum = fnv1a_64(csum, traps_p1);
    csum = fnv1a_64(csum, tsr_cause[0]);
    csum = fnv1a_64(csum, ecall_epc_ok_p1);
    csum = fnv1a_64(csum, (mstatus_pre_set >> 22) & 1UL);
    csum = fnv1a_64(csum, (mstatus_tsr_set >> 22) & 1UL);
    csum = fnv1a_64(csum, traps_p2);
    csum = fnv1a_64(csum, tsr_cause[traps_p1]);
    csum = fnv1a_64(csum, epc0_ok_p2);
    csum = fnv1a_64(csum, tsr_cause[traps_p1 + 1]);
    csum = fnv1a_64(csum, ecall_epc_ok_p2);
    csum = fnv1a_64(csum, (mstatus_tsr_clear >> 22) & 1UL);
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
