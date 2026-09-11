// ssw_main.c: sip.STIP software-write probe in S-mode (backlog item
// riscv sip-stip-write).
//
// The mechanism under test: with the Sstc extension present, bit 5 of
// sip (STIP) is writable in S-mode, so software can pend its own
// supervisor timer interrupt. The S-mode payload disarms stimecmp to
// all-ones first (so no real timer can fire), enables sie.STIE,
// writes STIP with csrs sip, and requires the readback to show STIP
// stuck; then sstatus.SIE is set inside the labeled wait region and
// exactly one trap must fire with scause 0x8000000000000005. The
// handler clears STIP with csrc sip, records the sip readback after
// the clear, and a bounded quiet window then requires zero
// re-delivery.
//
// Honesty fork: if the csrs write does not stick (readback shows STIP
// clear), the premise is not reproduced on this hart. The module does
// not force it: it publishes the readbacks, runs a quiet window with
// zero traps expected, and the verdict asserts the measured
// WARL-ignore result. The readback is the verdict either way.
//
// All waits are bounded by instruction budgets, never open ended, so
// a broken machine yields FAIL lines, not a hang. The
// verdict-relevant output lines carry register values and small
// counts only; the armed-value-free design means every printed value
// is a read or a counter. A 64-bit FNV-1a checksum over the
// deterministic measured values is printed and cross-checked in the
// PROOF.md results table.

#include "ssw.h"
#include "../uart.h"

ssw_save_t ssw_save;
volatile unsigned long ssw_trap_count;
volatile unsigned long ssw_scause;
volatile unsigned long ssw_sepc;
volatile unsigned long ssw_sip_after_clear;
volatile unsigned long ssw_bad_seen;
volatile unsigned long ssw_bad_scause;
volatile unsigned long ssw_bad_sepc;
volatile unsigned long ssw_stce;
volatile unsigned long ssw_mideleg;

static unsigned long m_scratch_area[4];

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

static unsigned long rd_time(void) {
    unsigned long v;
    __asm__ volatile("rdtime %0" : "=r"(v));
    return v;
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

static unsigned long read_stimecmp(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, 0x14d" : "=r"(v));
    return v;
}

extern void m_trap_entry(void);

// No M-mode trap is expected after boot. If one fires, print what it
// was and park the hart: the run fails via the timeout harness and the
// log shows the unexpected trap instead of a silent hang.
void m_unexpected_trap(void) {
    unsigned long mcause = m_scratch_area[1];
    unsigned long mepc = m_scratch_area[2];
    uart_puts("\nUNEXPECTED M-mode trap: mcause=");
    uart_put_hex(mcause);
    uart_puts(" mepc=");
    uart_put_hex(mepc);
    uart_puts("\nRESULT: FAIL (unexpected M-mode trap)\n");
    for (;;)
        __asm__ volatile("wfi");
}

// S-mode trap handler: runs on the dedicated trap stack with all
// registers saved. The first trap records scause/sepc, clears STIP
// (the source under test is the software write, not the timer, which
// stays disarmed), and records the sip readback after the clear so the
// drop of the pending bit is measured, not assumed. Any further trap
// (there must be none) is recorded as bad.
void ssw_trap_handler(ssw_save_t *s) {
    unsigned long after;
    if (ssw_trap_count == 0) {
        ssw_scause = s->scause;
        ssw_sepc = s->sepc;
        __asm__ volatile("csrc sip, %0" :: "r"(SIP_STIP)); // clear STIP
        __asm__ volatile("csrr %0, sip" : "=r"(after));
        ssw_sip_after_clear = after;
    } else {
        ssw_bad_seen = 1;
        ssw_bad_scause = s->scause;
        ssw_bad_sepc = s->sepc;
    }
    ssw_trap_count++;
}

// Wait loop, emitted verbatim: the SIE=1 transition happens exactly at
// the csrsi inside the labeled region, then spin on ssw_trap_count
// with a decrementing budget until the pending trap lands. Because
// STIP was already pending before the gate opens, the trap lands at
// an instruction boundary inside the ssw_loop/ssw_done region.
// Marked noinline so the labels are defined exactly once; sepc of the
// one trap must land between them. The region bounds are taken from
// in-asm numeric/global labels, never from a C labels-as-values
// address, so the trap-resume bounds are resolved exactly.
__attribute__((noinline)) static void open_gate_and_wait(void) {
    __asm__ volatile(
        ".global ssw_loop\n"
        "ssw_loop:\n"
        "csrsi sstatus, 2\n"   // open the gate: the pending trap can land here
        "mv t1, %1\n"
        "1:\n"
        "ld t0, 0(%0)\n"
        "bnez t0, 2f\n"
        "addi t1, t1, -1\n"
        "bnez t1, 1b\n"
        "2:\n"
        ".global ssw_done\n"
        "ssw_done:\n"
        :
        : "r"(&ssw_trap_count), "r"(SSW_TRAP_WAIT_READS)
        : "t0", "t1", "memory");
}

// Quiet window: bounded rdcycle reads with the gate open. No trap may
// fire. Marked noinline so the window is emitted exactly once.
static volatile unsigned long ssw_sink;
__attribute__((noinline)) static void quiet_window(void) {
    unsigned long i;
    for (i = 0; i < SSW_QUIET_READS; i++) {
        unsigned long v;
        __asm__ volatile("rdcycle %0" : "=r"(v));
        ssw_sink = v;
    }
}

static unsigned long fnv1a_64(unsigned long h, unsigned long v) {
    h ^= v;
    h *= 0x100000001b3UL;
    return h;
}

// FNV-1a 64-bit over the deterministic measured values: the sip
// readback right after the software write, the STIP-stuck flag, the
// trap count, the trap's scause, the sip readback the handler took
// after clearing STIP, the quiet-window extra-trap count, and the SIE
// bit readback after the release. sepc is excluded: it is diagnostic
// and depends on which wait-loop instruction the interrupt lands on.
static unsigned long measured_checksum(unsigned long sip_after_write,
                                       unsigned long stip_stuck,
                                       unsigned long quiet_extra,
                                       unsigned long sie_after) {
    unsigned long h = 0xcbf29ce484222325UL;
    h = fnv1a_64(h, sip_after_write);
    h = fnv1a_64(h, stip_stuck);
    h = fnv1a_64(h, ssw_trap_count);
    h = fnv1a_64(h, ssw_scause);
    h = fnv1a_64(h, ssw_sip_after_clear);
    h = fnv1a_64(h, quiet_extra);
    h = fnv1a_64(h, sie_after);
    return h;
}

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U

// S-mode body. Reached by mret from M-mode boot below.
void s_main(void) {
    unsigned long sip_after_write, sip_end, sie_after;
    unsigned long stip_stuck, traps_quiet_extra, csum;

    uart_puts("in S-mode: disarming stimecmp first, then sie.STIE on\n");

    // The disarm must come before any interrupt enable: stimecmp reads
    // all-ones here because M-mode parked it before the drop; write it
    // again explicitly so the source under test is the software write
    // alone, never the timer.
    __asm__ volatile("csrw 0x14d, %0" :: "r"(~0UL));
    check(read_stimecmp() == ~0UL, "stimecmp not all-ones at S-mode entry");

    // Enable only the supervisor timer interrupt.
    __asm__ volatile("csrs sie, %0" :: "r"(SIE_STIE));
    check((read_sie() & SIE_STIE) != 0, "sie.STIE did not stick");

    // SIE must arrive clear from the M-mode drop (SPIE was 0) and stay
    // clear until the software write is verified.
    __asm__ volatile("csrc sstatus, %0" :: "r"(SSTATUS_SIE));
    check((read_sstatus() & SSTATUS_SIE) == 0, "SIE not clear before STIP write");

    // The write under test: pend the supervisor timer interrupt by
    // software.
    uart_puts("writing STIP via csrs sip,0x20\n");
    __asm__ volatile("csrs sip, %0" :: "r"(SIP_STIP));
    sip_after_write = read_sip();
    stip_stuck = (sip_after_write & SIP_STIP) != 0;
    uart_puts("sip readback after write=");
    uart_put_hex(sip_after_write);
    uart_puts(" STIP-stuck=");
    uart_put_dec(stip_stuck);
    uart_puts("\n");

    if (!stip_stuck) {
        // Honest fork: the write did not stick, so the backlog premise
        // is not reproduced on this hart. Publish the readbacks and
        // assert the WARL-ignore result: no trap can come from a
        // pending bit that never pended.
        uart_puts("NOTE: STIP write ignored (WARL legalize-away); "
                  "no trap expected, quiet window only\n");
        check(ssw_trap_count == 0, "trap fired with STIP clear");
        quiet_window();
        uart_puts("quiet: traps=");
        uart_put_dec(ssw_trap_count);
        uart_puts(" (expect 0)\n");
        check(ssw_trap_count == 0, "trap fired during quiet window");
        csum = measured_checksum(sip_after_write, 0, 0, 0);
        uart_puts("VERDICT sip_after_write=");
        uart_put_hex(sip_after_write);
        uart_puts(" stip_stuck=0 traps=");
        uart_put_dec(ssw_trap_count);
        uart_puts(" checksum=");
        uart_put_hex(csum);
        uart_puts("\n");
        uart_puts("checks=");
        uart_put_dec(checks);
        uart_puts(" fails=");
        uart_put_dec(fails);
        uart_puts("\n");
        if (fails == 0)
            uart_puts("RESULT: PASS (premise not reproduced: write ignored)\n");
        else
            uart_puts("RESULT: FAIL\n");
        {
            unsigned long drain = rd_time();
            while (rd_time() - drain < 100000UL)
                ;
        }
        if (fails == 0) {
            *VIRT_TEST_FINISHER = FINISHER_PASS;
            for (;;)
                __asm__ volatile("wfi");
        }
        for (;;)
            __asm__ volatile("wfi");
    }

    // Main path: STIP stuck, so the interrupt is pending by software
    // write. The only SIE=1 transition of the run happens inside the
    // labeled wait region, so the one trap can only land there.
    uart_puts("STIP stuck: opening sstatus.SIE inside the labeled wait loop\n");
    open_gate_and_wait();

    // The one trap: scause must be the supervisor timer interrupt, its
    // sepc must land inside the wait loop, and the counter must read 1.
    uart_puts("trap1: scause=");
    uart_put_hex(ssw_scause);
    uart_puts(" sepc=");
    uart_put_hex(ssw_sepc);
    uart_puts(" traps=");
    uart_put_dec(ssw_trap_count);
    uart_puts("\n");
    uart_puts("wait loop bounds: ssw_loop=");
    uart_put_hex((unsigned long)ssw_loop);
    uart_puts(" ssw_done=");
    uart_put_hex((unsigned long)ssw_done);
    uart_puts(" (diagnostic: sepc varies by a few instructions per run)\n");
    check(ssw_trap_count == 1, "trap never fired after SIE set");
    // sret restores SIE from SPIE, which the trap entry saved as 1, so
    // the gate is open again here.
    sie_after = (read_sstatus() & SSTATUS_SIE) != 0;
    check(sie_after, "SIE not open after the gate-open trap");
    check(ssw_scause == SCAUSE_STI, "scause != supervisor timer interrupt");
    check(ssw_sepc >= (unsigned long)ssw_loop &&
          ssw_sepc < (unsigned long)ssw_done,
          "sepc outside the wait loop");

    // The in-handler clear must have dropped the pending bit: the sip
    // readback the handler took after csrc must have STIP clear.
    uart_puts("clear: handler sip readback=");
    uart_put_hex(ssw_sip_after_clear);
    uart_puts(" (expect STIP clear)\n");
    check((ssw_sip_after_clear & SIP_STIP) == 0,
          "sip STIP still pending after handler clear");

    // Quiet window: the gate is open, the source is the software write
    // which the handler cleared, and stimecmp stays disarmed, so
    // nothing may fire. The counter must stay at 1.
    uart_puts("quiet window: ");
    uart_put_dec(SSW_QUIET_READS);
    uart_puts(" rdcycle reads with SIE on, STIP cleared\n");
    quiet_window();
    traps_quiet_extra = ssw_trap_count - 1; // traps beyond the one expected
    uart_puts("quiet: traps=");
    uart_put_dec(ssw_trap_count);
    uart_puts(" (expect 1)\n");
    check(ssw_trap_count == 1, "re-delivery fired during the quiet window");

    if (ssw_bad_seen) {
        uart_puts("unexpected extra trap: scause=");
        uart_put_hex(ssw_bad_scause);
        uart_puts(" sepc=");
        uart_put_hex(ssw_bad_sepc);
        uart_puts("\n");
    }

    sip_end = read_sip();
    uart_puts("end: sip=");
    uart_put_hex(sip_end);
    uart_puts(" STIP-clear=");
    uart_put_dec((sip_end & SIP_STIP) == 0 ? 1UL : 0UL);
    uart_puts("\n");
    check((sip_end & SIP_STIP) == 0, "sip STIP set at end of run");

    csum = measured_checksum(sip_after_write, stip_stuck,
                             traps_quiet_extra, sie_after);

    // Verdict section: deterministic measured values only. sepc is
    // diagnostic above, never part of the verdict or the checksum.
    uart_puts("VERDICT sip_after_write=");
    uart_put_hex(sip_after_write);
    uart_puts(" stip_stuck=");
    uart_put_dec(stip_stuck);
    uart_puts(" traps=");
    uart_put_dec(ssw_trap_count);
    uart_puts(" quiet_extra_traps=");
    uart_put_dec(traps_quiet_extra);
    uart_puts(" scause=");
    uart_put_hex(ssw_scause);
    uart_puts(" sip_after_clear=");
    uart_put_hex(ssw_sip_after_clear);
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

// M-mode boot. boot.S jumps here in M-mode.
int main(void) {
    unsigned long v;

    uart_init();
    uart_puts("sip-stip-write: S-mode sip.STIP software-write probe\n");

    // Probe for Sstc: set menvcfg.STCE and check the bit sticks.
    // S-mode access to stimecmp faults without it, and STIP is only
    // software-writable in S-mode with Sstc, so an absent extension
    // fails the run here rather than as a confusing trap.
    __asm__ volatile("csrs menvcfg, %0" :: "r"(MENVCFG_STCE));
    __asm__ volatile("csrr %0, menvcfg" : "=r"(v));
    ssw_stce = (v & MENVCFG_STCE) != 0;
    uart_puts("Sstc probe: menvcfg.STCE ");
    uart_puts(ssw_stce ? "sticks (present)" : "does not stick (absent)");
    uart_puts("\n");
    check(ssw_stce, "Sstc not present on this hart");

    // Disarm both comparators before any interrupt is enabled. The
    // machine timer stays parked for the whole run; the interrupt
    // under test is pended by a software sip write, never by the
    // timer.
    *(volatile unsigned long *)CLINT_MTIMECMP = ~0UL;
    __asm__ volatile("csrw 0x14d, %0" :: "r"(~0UL));

    // PMP: with no PMP entry programmed, S-mode has no access to any
    // address (M-mode keeps full access, lower modes default-deny).
    // Open the whole address space to S-mode with one NAPOT entry,
    // R/W/X, before the drop.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // mcounteren: time/cycle are unreadable in S-mode unless M-mode
    // grants access; the S-mode phases use rdtime and rdcycle.
    __asm__ volatile("csrw mcounteren, %0" :: "r"(0x7UL));  // CY|TM|IR

    // Delegate only the supervisor timer interrupt to S-mode.
    __asm__ volatile("csrw mideleg, %0" :: "r"(MIDELEG_STI));
    __asm__ volatile("csrr %0, mideleg" : "=r"(ssw_mideleg));
    uart_puts("mideleg=");
    uart_put_hex(ssw_mideleg);
    uart_puts("\n");
    check((ssw_mideleg & MIDELEG_STI) != 0, "mideleg STI bit did not stick");

    // M-mode keeps a minimal vector that reports any unexpected M-mode
    // trap and parks; S-mode gets the full vector.
    __asm__ volatile("csrw mtvec, %0" :: "r"(m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(m_scratch_area));
    __asm__ volatile("csrw stvec, %0" :: "r"(ssw_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"(&ssw_save));
    __asm__ volatile("csrr %0, stvec" : "=r"(v));
    check((v & 3UL) == 0, "stvec not in direct mode");

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
