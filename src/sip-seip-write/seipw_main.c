// seipw_main.c: S-mode sip.SEIP read-only probe (backlog item
// riscv sip-seip-write).
//
// The mechanism under test: sip.SEIP (bit 9) is read-only for S-mode.
// An S-mode write of all ones to sip must leave SEIP exactly as it
// was, while the software-writable SSIP bit (bit 1) reads back set,
// which proves the write really executed rather than being dropped
// wholesale. A zero write must clear the writable bits with SEIP
// still unchanged. Only the supervisor software interrupt is
// delegated (mideleg bit 1), which is what makes the S-mode SSIP
// write observable on this QEMU; SEI (bit 9) is not delegated, and
// a counting M-mode handler records mcause/mepc with its count
// required to stay 0, so the readbacks cannot be explained by a
// trap rewriting state. sie and sstatus.SIE stay clear for the
// whole run, so no pending bit can be taken as an interrupt either.
//
// The exact legalized value is asserted after both writes: the
// readback must equal the before-value with only the observed
// writable bits changed. STIP behavior is measured and reported as
// observed (the sip-stip-write sibling measured it legalized away on
// this QEMU); it is not assumed.
//
// All waits are bounded by instruction budgets, never open ended.
// The verdict-relevant output lines carry register values and small
// counts only. A 64-bit FNV-1a checksum over the deterministic
// measured values is printed and cross-checked in the PROOF.md
// results table.

#include "seipw.h"
#include "../uart.h"

// M-mode trap save area; offsets match seipw_trap.S.
static unsigned long m_scratch_area[8];

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

static unsigned long read_sip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, sip" : "=r"(v));
    return v;
}

static unsigned long read_sie(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, sie" : "=r"(v));
    return v;
}

static unsigned long read_sstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    return v;
}

static unsigned long rd_time(void) {
    unsigned long v;
    __asm__ volatile("rdtime %0" : "=r"(v));
    return v;
}

static unsigned long trap_count(void) {
    return m_scratch_area[SEIPW_COUNT_OFF / 8];
}

static unsigned long fnv1a_64(unsigned long h, unsigned long v) {
    h ^= v;
    h *= 0x100000001b3UL;
    return h;
}

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U

static void verdict_tail(unsigned long sip_before, unsigned long sip_after,
                         unsigned long sip_zeroed, unsigned long sie_at_entry,
                         unsigned long ssip_stuck, unsigned long stip_stuck) {
    unsigned long csum = 0xcbf29ce484222325UL;

    // FNV-1a over the observed (write, readback, trap-count) triples
    // plus the entry-state reads: deterministic on this board.
    csum = fnv1a_64(csum, ~0UL);       // the all-ones write value
    csum = fnv1a_64(csum, sip_before);
    csum = fnv1a_64(csum, sip_after);
    csum = fnv1a_64(csum, 0UL);        // the zero write value
    csum = fnv1a_64(csum, sip_zeroed);
    csum = fnv1a_64(csum, trap_count());
    csum = fnv1a_64(csum, sie_at_entry);
    csum = fnv1a_64(csum, ssip_stuck);
    csum = fnv1a_64(csum, stip_stuck);

    uart_puts("VERDICT sip_before=");
    uart_put_hex(sip_before);
    uart_puts(" sip_after=");
    uart_put_hex(sip_after);
    uart_puts(" sip_zeroed=");
    uart_put_hex(sip_zeroed);
    uart_puts(" traps=");
    uart_put_dec(trap_count());
    uart_puts(" checksum=");
    uart_put_hex(csum);
    uart_puts("\n");
    uart_puts("checks=");
    uart_put_dec(checks);
    uart_puts(" fails=");
    uart_put_dec(fails);
    uart_puts("\n");

    if (fails == 0)
        uart_puts("RESULT: PASS\n");
    else {
        uart_puts("RESULT: FAIL (");
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
        *VIRT_TEST_FINISHER = FINISHER_PASS; // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    // FAIL: park the hart without touching the finisher device. The
    // harness runs QEMU under timeout, so a FAIL is observable as
    // the timeout exit status (124) as well as the RESULT line.
    for (;;)
        __asm__ volatile("wfi");
}

// S-mode body. Reached by mret from M-mode boot below.
void s_main(void) {
    unsigned long sip_before, sip_after, sip_zeroed;
    unsigned long sie_at_entry;
    unsigned long ssip_stuck, stip_stuck;
    unsigned long expected_after, expected_zeroed;

    uart_puts("in S-mode: only SSI delegated (mideleg bit 1); SEI stays in M-mode\n");

    // The gate stays shut for the whole run: with SIE clear no
    // pending bit can be taken as an interrupt.
    __asm__ volatile("csrc sstatus, %0" :: "r"(SSTATUS_SIE));
    check((read_sstatus() & SSTATUS_SIE) == 0, "SIE not clear at S-mode entry");

    sie_at_entry = read_sie();
    uart_puts("sie at entry=");
    uart_put_hex(sie_at_entry);
    uart_puts("\n");
    check(sie_at_entry == 0, "sie nonzero at S-mode entry");

    // The before readback. SEIP must be 0: no UART/PLIC interrupt is
    // asserted on the quiet board.
    sip_before = read_sip();
    uart_puts("sip before=");
    uart_put_hex(sip_before);
    uart_puts("\n");

    if ((sip_before & SIP_SEIP) != 0) {
        // Honest abort: an external interrupt is asserted at boot, so
        // the "SEIP reads 0 throughout" premise does not hold on this
        // run. Do not force it; report and stop.
        uart_puts("NOTE: SEIP is set before any write, so an external "
                  "interrupt is asserted on this board. The read-only "
                  "premise cannot be shown against a set bit; aborting.\n");
        uart_puts("RESULT: FAIL (premise not met: SEIP asserted at boot)\n");
        for (;;)
            __asm__ volatile("wfi");
    }
    check((sip_before & SIP_SEIP) == 0, "SEIP set before the write");

    // The write under test: all ones to sip, from S-mode.
    uart_puts("writing all-ones to sip from S-mode\n");
    __asm__ volatile("csrw sip, %0" :: "r"(~0UL));
    sip_after = read_sip();
    uart_puts("sip after all-ones=");
    uart_put_hex(sip_after);
    uart_puts("\n");

    ssip_stuck = (sip_after & SIP_SSIP) != 0;
    stip_stuck = (sip_after & SIP_STIP) != 0;
    uart_puts("SSIP-stuck=");
    uart_put_dec(ssip_stuck);
    uart_puts(" STIP-stuck=");
    uart_put_dec(stip_stuck);
    uart_puts("\n");
    if (!stip_stuck)
        uart_puts("NOTE: STIP legalized away on this hart (matches the "
                  "sip-stip-write sibling); SSIP is the write-executed proof\n");

    // The core claim: the read-only SEIP bit is unchanged by the
    // all-ones write.
    check(((sip_after ^ sip_before) & SIP_SEIP) == 0,
          "SEIP changed by the S-mode all-ones write");
    check((sip_after & SIP_SEIP) == 0, "SEIP set after the all-ones write");

    // The write really executed: a software-writable bit stuck.
    check(ssip_stuck, "SSIP did not stick; the write is unobservable");

    // Exact legalization: only the bits observed writable may differ
    // from the before value.
    expected_after = sip_before |
                     (ssip_stuck ? SIP_SSIP : 0UL) |
                     (stip_stuck ? SIP_STIP : 0UL);
    check(sip_after == expected_after,
          "sip readback is not the exact legalized value");

    // The control: no trap may have fired on the S-mode CSR writes.
    uart_puts("M-mode trap count after the writes=");
    uart_put_dec(trap_count());
    uart_puts(" (expect 0)\n");
    check(trap_count() == 0, "trap fired on the S-mode sip writes");

    // The zero write: writable bits must clear, SEIP still unchanged.
    uart_puts("writing zero to sip from S-mode\n");
    __asm__ volatile("csrw sip, %0" :: "r"(0UL));
    sip_zeroed = read_sip();
    uart_puts("sip after zero=");
    uart_put_hex(sip_zeroed);
    uart_puts("\n");

    check((sip_zeroed & SIP_SSIP) == 0, "SSIP still set after the zero write");
    check((sip_zeroed & SIP_STIP) == 0, "STIP still set after the zero write");
    check(((sip_zeroed ^ sip_before) & SIP_SEIP) == 0,
          "SEIP changed by the S-mode zero write");
    expected_zeroed = sip_before & ~(SIP_SSIP | SIP_STIP);
    check(sip_zeroed == expected_zeroed,
          "sip readback after zero is not the exact legalized value");

    if (trap_count() != 0) {
        uart_puts("surprise M-mode trap: mcause=");
        uart_put_hex(m_scratch_area[SEIPW_MCAUSE_OFF / 8]);
        uart_puts(" mepc=");
        uart_put_hex(m_scratch_area[SEIPW_MEPC_OFF / 8]);
        uart_puts("\n");
    }
    check(trap_count() == 0, "trap fired during the run");

    verdict_tail(sip_before, sip_after, sip_zeroed, sie_at_entry,
                 ssip_stuck, stip_stuck);
}

// M-mode boot. boot.S jumps here in M-mode.
int main(void) {
    unsigned long v;

    uart_init();
    uart_puts("sip-seip-write: S-mode sip.SEIP read-only probe\n");

    // Install the counting M-mode vector first, so any surprise from
    // here on is recorded instead of hanging silently.
    __asm__ volatile("csrw mtvec, %0" :: "r"(m_trap_entry));
    __asm__ volatile("csrr %0, mtvec" : "=r"(v));
    check((v & 3UL) == 0, "mtvec not in direct mode");
    __asm__ volatile("csrw mscratch, %0" :: "r"(m_scratch_area));

    // Park the machine timer comparator so no timer interrupt can be
    // involved in this run at all.
    *(volatile unsigned long *)CLINT_MTIMECMP = ~0UL;

    // Sstc probe: STCE is the menvcfg bit that exists only with Sstc.
    __asm__ volatile("csrs menvcfg, %0" :: "r"(MENVCFG_STCE));
    __asm__ volatile("csrr %0, menvcfg" : "=r"(v));
    uart_puts("Sstc probe: menvcfg.STCE ");
    uart_puts((v & MENVCFG_STCE) ? "sticks (present)" : "does not stick (absent)");
    uart_puts("\n");
    check((v & MENVCFG_STCE) != 0, "Sstc not present on this hart");

    // PMP: with no PMP entry programmed, S-mode has no access to any
    // address (M-mode keeps full access, lower modes default-deny).
    // Open the whole address space to S-mode with one NAPOT entry,
    // R/W/X, before the drop.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(v));
    check(v == 0x1f, "pmpcfg0 did not open NAPOT R/W/X");

    // mcounteren: time is unreadable in S-mode unless M-mode grants
    // access; the drain loop uses rdtime.
    __asm__ volatile("csrw mcounteren, %0" :: "r"(0x7UL));  // CY|TM|IR

    // Delegate ONLY the supervisor software interrupt (mideleg bit
    // 1), so the S-mode SSIP write is observable: this QEMU drops
    // S-mode sip writes entirely when the bit is not delegated
    // (measured on the first bring-up run), which would confound
    // "SEIP ignored" with "write dropped". Bits 5 (STI) and 9 (SEI)
    // stay clear, so timer and external traps keep their M-mode
    // path, and sie plus sstatus.SIE stay clear, so the pended SSIP
    // is never taken as a trap.
    __asm__ volatile("csrw mideleg, %0" :: "r"(MIDELEG_SSI));
    __asm__ volatile("csrr %0, mideleg" : "=r"(v));
    uart_puts("mideleg=");
    uart_put_hex(v);
    uart_puts("\n");
    check((v & (MIDELEG_SSI | MIDELEG_STI | MIDELEG_SEI)) == MIDELEG_SSI,
          "mideleg is not exactly SSI-delegated");

    // Drop to S-mode at s_main with mret: MPP=01 selects S-mode.
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    v = (v & ~(3UL << 11)) | (1UL << 11);
    __asm__ volatile("csrw mstatus, %0" :: "r"(v));
    uart_puts("dropping to S-mode\n");
    __asm__ volatile("la t0, s_main\n"
                     "csrw mepc, t0\n"
                     "mret");
    for (;;)
        __asm__ volatile("wfi");
}
