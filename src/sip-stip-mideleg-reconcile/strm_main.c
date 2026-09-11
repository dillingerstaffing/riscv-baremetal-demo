// strm_main.c: sip.STIP vs mideleg bit 5 reconciliation probe
// (backlog item riscv sip-stip-mideleg-reconcile).
//
// The question: two earlier modules measured the S-mode software
// write to sip bit 5 (STIP) differently on the surface. This module
// runs the same S-mode all-ones sip write twice in one boot:
// Phase A with mideleg bit 5 (STI delegation) CLEAR, Phase B with
// mideleg bit 5 SET (raised by the M-mode trap handler in response
// to the deliberate ecall that ends Phase A). It publishes the sip
// readback after the write and after a zero write in each
// configuration, plus the mideleg readback in each configuration,
// and requires zero unexpected traps throughout.
//
// The write-executed proof in each phase is SSIP (bit 1), which is
// delegated in both phases: on this QEMU an S-mode sip write only
// admits bits whose interrupt is delegated via mideleg (measured by
// the sip-seip-write sibling), so a stuck SSIP shows the write
// executed and a clear STIP means legalized away, not dropped
// wholesale. sie and sstatus.SIE stay clear for the whole run, so
// no pending bit can be taken as an interrupt either.
//
// All waits are bounded by instruction budgets, never open ended.
// The verdict-relevant output lines carry register values and small
// counts only. A 64-bit FNV-1a checksum over the deterministic
// measured values is printed and cross-checked in the PROOF.md
// results table.

#include "strm.h"
#include "../uart.h"

// M-mode trap save area; offsets match strm_trap.S.
static unsigned long m_scratch_area[8];

// Written by the M-mode trap handler when it recognizes the
// deliberate end-of-Phase-A ecall: the Phase B mideleg readback and
// the phase_b flag.
volatile unsigned long mideleg_phase_b = 0;
volatile unsigned long phase_b = 0;

// Phase A mideleg readback, captured by M-mode boot before the drop.
static unsigned long mideleg_phase_a = 0;

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
    return m_scratch_area[STRM_COUNT_OFF / 8];
}

static unsigned long fnv1a_64(unsigned long h, unsigned long v) {
    h ^= v;
    h *= 0x100000001b3UL;
    return h;
}

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U

struct phase_out {
    unsigned long before;
    unsigned long after;
    unsigned long zeroed;
    unsigned long ssip_stuck;
    unsigned long stip_stuck;
};

// One configuration's probes: publish the before readback, write
// all-ones, publish the after readback and the stuck flags, write
// zero, publish the zeroed readback, and require the trap count to
// be exactly expect_traps. The exact legalized readback is asserted
// from the observed stuck flags: only bits observed writable may
// differ from the before value.
static struct phase_out run_phase(const char *label, unsigned long expect_traps) {
    struct phase_out o;
    unsigned long expected;

    uart_puts(label);
    uart_puts(" sip before=");
    o.before = read_sip();
    uart_put_hex(o.before);
    uart_puts("\n");
    check(o.before == 0, "sip nonzero at phase entry");

    uart_puts(label);
    uart_puts(" writing all-ones to sip from S-mode\n");
    __asm__ volatile("csrw sip, %0" :: "r"(~0UL));
    o.after = read_sip();
    uart_puts(label);
    uart_puts(" sip after all-ones=");
    uart_put_hex(o.after);
    uart_puts("\n");

    o.ssip_stuck = (o.after & SIP_SSIP) != 0;
    o.stip_stuck = (o.after & SIP_STIP) != 0;
    uart_puts(label);
    uart_puts(" SSIP-stuck=");
    uart_put_dec(o.ssip_stuck);
    uart_puts(" STIP-stuck=");
    uart_put_dec(o.stip_stuck);
    uart_puts("\n");

    // The write really executed: a software-writable bit stuck. A
    // clear STIP is therefore legalized away, not a dropped write.
    check(o.ssip_stuck, "SSIP did not stick; the write is unobservable");

    // Exact legalization: only the bits observed writable may differ
    // from the before value.
    expected = o.before |
               (o.ssip_stuck ? SIP_SSIP : 0UL) |
               (o.stip_stuck ? SIP_STIP : 0UL);
    check(o.after == expected,
          "sip readback is not the exact legalized value");

    uart_puts(label);
    uart_puts(" M-mode trap count=");
    uart_put_dec(trap_count());
    uart_puts(" (expect ");
    uart_put_dec(expect_traps);
    uart_puts(")\n");
    check(trap_count() == expect_traps, "unexpected trap during the phase");

    uart_puts(label);
    uart_puts(" writing zero to sip from S-mode\n");
    __asm__ volatile("csrw sip, %0" :: "r"(0UL));
    o.zeroed = read_sip();
    uart_puts(label);
    uart_puts(" sip after zero=");
    uart_put_hex(o.zeroed);
    uart_puts("\n");

    check((o.zeroed & SIP_SSIP) == 0, "SSIP still set after the zero write");
    check((o.zeroed & SIP_STIP) == 0, "STIP still set after the zero write");
    expected = o.before & ~(SIP_SSIP | SIP_STIP);
    check(o.zeroed == expected,
          "sip readback after zero is not the exact legalized value");

    return o;
}

static void verdict_tail(struct phase_out a, struct phase_out b,
                         unsigned long sie_at_entry) {
    unsigned long csum = 0xcbf29ce484222325UL;

    // FNV-1a over the measured values: deterministic on this board.
    csum = fnv1a_64(csum, mideleg_phase_a);
    csum = fnv1a_64(csum, mideleg_phase_b);
    csum = fnv1a_64(csum, sie_at_entry);
    csum = fnv1a_64(csum, ~0UL);       // the all-ones write value
    csum = fnv1a_64(csum, a.before);
    csum = fnv1a_64(csum, a.after);
    csum = fnv1a_64(csum, a.ssip_stuck);
    csum = fnv1a_64(csum, a.stip_stuck);
    csum = fnv1a_64(csum, 0UL);        // the zero write value
    csum = fnv1a_64(csum, a.zeroed);
    csum = fnv1a_64(csum, b.before);
    csum = fnv1a_64(csum, b.after);
    csum = fnv1a_64(csum, b.ssip_stuck);
    csum = fnv1a_64(csum, b.stip_stuck);
    csum = fnv1a_64(csum, b.zeroed);
    csum = fnv1a_64(csum, trap_count());
    csum = fnv1a_64(csum, m_scratch_area[STRM_MCAUSE_OFF / 8]);

    uart_puts("VERDICT mideleg_A=");
    uart_put_hex(mideleg_phase_a);
    uart_puts(" mideleg_B=");
    uart_put_hex(mideleg_phase_b);
    uart_puts(" sipA_after=");
    uart_put_hex(a.after);
    uart_puts(" stipA_stuck=");
    uart_put_dec(a.stip_stuck);
    uart_puts(" sipB_after=");
    uart_put_hex(b.after);
    uart_puts(" stipB_stuck=");
    uart_put_dec(b.stip_stuck);
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
    struct phase_out a, b;
    unsigned long sie_at_entry;

    uart_puts("in S-mode: Phase A, mideleg bit 5 (STI) CLEAR; SSI delegated\n");

    // The gate stays shut for the whole run: with SIE clear no
    // pending bit can be taken as an interrupt.
    __asm__ volatile("csrc sstatus, %0" :: "r"(SSTATUS_SIE));
    check((read_sstatus() & SSTATUS_SIE) == 0, "SIE not clear at S-mode entry");

    sie_at_entry = read_sie();
    uart_puts("sie at entry=");
    uart_put_hex(sie_at_entry);
    uart_puts("\n");
    check(sie_at_entry == 0, "sie nonzero at S-mode entry");

    a = run_phase("phaseA", 0);

    // End of Phase A: a deliberate ecall. The M-mode handler sets
    // mideleg bit 5 and records the new readback before mret
    // resumes here in S-mode.
    uart_puts("phaseA done; ecall to M-mode to delegate STI for phase B\n");
    __asm__ volatile("ecall");
    __asm__ volatile("" ::: "memory");

    check(phase_b == 1, "phase_b flag not raised by the trap handler");
    uart_puts("phase B mideleg=");
    uart_put_hex(mideleg_phase_b);
    uart_puts("\n");

    // Only the deliberate ecall may have fired.
    uart_puts("M-mode trap count after the ecall=");
    uart_put_dec(trap_count());
    uart_puts(" (expect 1)\n");
    check(trap_count() == 1, "trap count is not exactly the deliberate ecall");
    uart_puts("recorded mcause=");
    uart_put_hex(m_scratch_area[STRM_MCAUSE_OFF / 8]);
    uart_puts(" (expect 9: S-mode ecall)\n");
    check(m_scratch_area[STRM_MCAUSE_OFF / 8] == 9,
          "the one trap is not the S-mode ecall");

    // The handler must have raised exactly mideleg bit 5 on top of
    // the Phase A delegation.
    check((mideleg_phase_b & (MIDELEG_SSI | MIDELEG_STI | MIDELEG_SEI)) ==
          (MIDELEG_SSI | MIDELEG_STI),
          "Phase B mideleg is not exactly SSI+STI delegated");

    b = run_phase("phaseB", 1);

    // The reconciliation: the STIP-stuck flag must be identical in
    // both configurations. STI delegation does not change whether
    // an S-mode software write pends STIP on this hart.
    uart_puts("reconciliation: STIP-stuck phaseA=");
    uart_put_dec(a.stip_stuck);
    uart_puts(" phaseB=");
    uart_put_dec(b.stip_stuck);
    uart_puts("\n");
    check(a.stip_stuck == b.stip_stuck,
          "STIP writability differs between the two mideleg configurations");

    if (trap_count() != 1) {
        uart_puts("surprise M-mode trap: mcause=");
        uart_put_hex(m_scratch_area[STRM_MCAUSE_OFF / 8]);
        uart_puts(" mepc=");
        uart_put_hex(m_scratch_area[STRM_MEPC_OFF / 8]);
        uart_puts("\n");
    }
    check(trap_count() == 1, "trap fired during Phase B");

    verdict_tail(a, b, sie_at_entry);
}

// M-mode boot. boot.S jumps here in M-mode.
int main(void) {
    unsigned long v;

    uart_init();
    uart_puts("sip-stip-mideleg-reconcile: STIP write vs mideleg bit 5\n");

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

    // Phase A: delegate ONLY the supervisor software interrupt
    // (mideleg bit 1), so the S-mode sip writes are observable: this
    // QEMU admits an S-mode sip write only for bits whose interrupt
    // is delegated (measured by the sip-seip-write sibling). Bit 5
    // (STI) stays clear in Phase A; the trap handler will set it
    // after the deliberate ecall. Bits 2, 6, 8, 10 are this
    // emulator's stuck reset bits (0x1444); the check asserts only
    // the bits that matter.
    __asm__ volatile("csrw mideleg, %0" :: "r"(MIDELEG_SSI));
    __asm__ volatile("csrr %0, mideleg" : "=r"(v));
    mideleg_phase_a = v;
    uart_puts("phaseA mideleg=");
    uart_put_hex(v);
    uart_puts("\n");
    check((v & (MIDELEG_SSI | MIDELEG_STI | MIDELEG_SEI)) == MIDELEG_SSI,
          "Phase A mideleg is not exactly SSI-delegated");

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
