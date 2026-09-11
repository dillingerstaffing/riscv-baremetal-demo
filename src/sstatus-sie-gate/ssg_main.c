// ssg_main.c: sstatus.SIE as the S-mode interrupt gate (backlog item
// 176).
//
// The fundamental truth under test: sstatus.SIE gates supervisor-mode
// interrupt delivery independently of the pending bits. A pended
// supervisor timer interrupt stays pending without trapping while
// SIE=0, and must trap the moment SIE=1.
//
// Phase A (gate closed, runs in S-mode with SIE clear):
//   1. sie.STIE is enabled; the arm write is stimecmp = mtime +
//      SSG_AHEAD_TICKS. The gate under test, sstatus.SIE, starts CLEAR.
//   2. Poll a bounded window of SSG_GATED_READS rdcycle reads. mtime
//      runs past stimecmp inside the window, so sip STIP must read 1
//      (the interrupt is pending) while the trap counter stays at 0
//      (the gate is closed).
// Phase B (gate open):
//   3. csrsi sstatus, 2 (SIE=1). The pending interrupt must trap.
//      The handler records scause (expect 0x8000000000000005, the
//      supervisor timer interrupt) and sepc, then disarms stimecmp to
//      all-ones so the level-triggered source drops and no re-delivery
//      can occur.
//   4. Quiet window with SIE still on: SSG_QUIET_READS rdcycle reads;
//      the trap counter must stay at 1, sip STIP must read 0, and
//      stimecmp must still read all-ones.
//
// All waits are bounded by instruction or mtime budgets, never open
// ended, so a broken machine yields FAIL lines, not a hang. The
// verdict-relevant output lines carry register values and small counts
// only; host-timing-dependent values (the armed stimecmp, rdcycle
// deltas) are printed as match booleans or marked diagnostic. A 64-bit
// FNV-1a checksum over the deterministic measured values is printed
// and cross-checked in the PROOF.md results table.

#include "ssg.h"
#include "../uart.h"

ssg_save_t ssg_save;
volatile unsigned long ssg_trap_count;
volatile unsigned long ssg_scause;
volatile unsigned long ssg_sepc;
volatile unsigned long ssg_stip_gated;
volatile unsigned long ssg_bad_seen;
volatile unsigned long ssg_bad_scause;
volatile unsigned long ssg_bad_sepc;
volatile unsigned long ssg_stce;
volatile unsigned long ssg_mideleg;

static unsigned long m_scratch_area[4];
static unsigned long gated_reads_done;

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

static unsigned long read_stimecmp(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, 0x14d" : "=r"(v));
    return v;
}

static unsigned long read_sip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, sip" : "=r"(v));
    return v;
}

static unsigned long read_sstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
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
// registers saved. The first trap records scause/sepc and disarms the
// timer (stimecmp = all-ones) so the level-triggered source drops and
// the trap cannot re-fire. Any further trap (there must be none) is
// recorded as bad.
void ssg_trap_handler(ssg_save_t *s) {
    if (ssg_trap_count == 0) {
        ssg_scause = s->scause;
        ssg_sepc = s->sepc;
        __asm__ volatile("csrw 0x14d, %0" :: "r"(~0UL)); // disarm
    } else {
        ssg_bad_seen = 1;
        ssg_bad_scause = s->scause;
        ssg_bad_sepc = s->sepc;
    }
    ssg_trap_count++;
}

// Phase-A gated window, emitted verbatim: with SIE known clear, spin
// SSG_GATED_READS rdcycle reads, watching sip STIP. The volatile sink
// keeps the loop from being folded away. Marked noinline so the window
// is emitted exactly once; the trap counter must not move here.
static volatile unsigned long ssg_sink;
__attribute__((noinline)) static void gated_window(void) {
    unsigned long i;
    for (i = 0; i < SSG_GATED_READS; i++) {
        unsigned long v, sip;
        __asm__ volatile("rdcycle %0" : "=r"(v));
        __asm__ volatile("csrr %0, sip" : "=r"(sip));
        if (sip & SIP_STIP)
            ssg_stip_gated = 1;
        ssg_sink = v;
    }
    gated_reads_done = SSG_GATED_READS;
}

// Phase-B wait loop, emitted verbatim: the gate opens exactly at the
// csrsi inside the labeled region, then spin on ssg_trap_count with a
// decrementing budget until the pending trap lands. Because the
// interrupt was already pending before the gate opens, the trap lands
// at an instruction boundary inside the ssg_loop/ssg_done region.
// Marked noinline so the labels are defined exactly once; sepc of the
// one trap must land between them.
__attribute__((noinline)) static void open_gate_and_wait(void) {
    __asm__ volatile(
        ".global ssg_loop\n"
        "ssg_loop:\n"
        "csrsi sstatus, 2\n"   // open the gate: the pending trap can land here
        "mv t1, %1\n"
        "1:\n"
        "ld t0, 0(%0)\n"
        "bnez t0, 2f\n"
        "addi t1, t1, -1\n"
        "bnez t1, 1b\n"
        "2:\n"
        ".global ssg_done\n"
        "ssg_done:\n"
        :
        : "r"(&ssg_trap_count), "r"(SSG_TRAP_WAIT_READS)
        : "t0", "t1", "memory");
}

// Quiet window: SIE on, sie STIE on, stimecmp disarmed. SSG_QUIET_READS
// rdcycle reads must produce zero traps. Marked noinline like the
// other windows.
__attribute__((noinline)) static void quiet_window(void) {
    unsigned long i;
    for (i = 0; i < SSG_QUIET_READS; i++) {
        unsigned long v;
        __asm__ volatile("rdcycle %0" : "=r"(v));
        ssg_sink = v;
    }
}

static unsigned long fnv1a_64(unsigned long h, unsigned long v) {
    h ^= v;
    h *= 0x100000001b3UL;
    return h;
}

// FNV-1a 64-bit over the deterministic measured values only: trap
// count in the gated window, the gated sip-STIP observation, trap
// count after the gate opened, the trap's scause, the final stimecmp
// readback, the final sip-STIP bit, the quiet-window trap count, and
// the SIE bit readback after the release. sepc and the armed stimecmp
// value are excluded: both are diagnostic and host-timing dependent,
// so they must not enter the checksum.
static unsigned long measured_checksum(unsigned long traps_gated,
                                       unsigned long traps_quiet,
                                       unsigned long cmp_final,
                                       unsigned long sip_final,
                                       unsigned long sie_after) {
    unsigned long h = 0xcbf29ce484222325UL;
    h = fnv1a_64(h, traps_gated);
    h = fnv1a_64(h, ssg_stip_gated);
    h = fnv1a_64(h, ssg_trap_count);
    h = fnv1a_64(h, ssg_scause);
    h = fnv1a_64(h, cmp_final);
    h = fnv1a_64(h, sip_final);
    h = fnv1a_64(h, traps_quiet);
    h = fnv1a_64(h, sie_after);
    return h;
}

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U

// S-mode body. Reached by mret from M-mode boot below.
void s_main(void) {
    unsigned long cmp_boot, cmp_programmed, cmp_final, sip_final;
    unsigned long sie_after, traps_gated, traps_quiet, csum;

    uart_puts("in S-mode: enabling sie.STIE, keeping sstatus.SIE clear\n");

    // SIE must arrive clear from the M-mode drop (SPIE was 0), and the
    // gate under test stays closed explicitly.
    __asm__ volatile("csrc sstatus, %0" :: "r"(SSTATUS_SIE));
    check((read_sstatus() & SSTATUS_SIE) == 0, "SIE not clear at phase A");

    // The M-mode boot disarm must be visible here: stimecmp reads
    // all-ones before S-mode ever touches it.
    cmp_boot = read_stimecmp();
    uart_puts("stimecmp at S-mode entry=");
    uart_put_hex(cmp_boot);
    uart_puts("\n");
    check(cmp_boot == ~0UL, "stimecmp not all-ones at S-mode entry");

    // Enable only the supervisor timer interrupt. SIE is the only gate.
    __asm__ volatile("csrs sie, %0" :: "r"(SIE_STIE));

    uart_puts("phase A: arming stimecmp = mtime+");
    uart_put_dec(SSG_AHEAD_TICKS);
    uart_puts(" ticks with SIE clear\n");

    // Arm the timer. The programmed value depends on boot-time mtime,
    // so the printed line reports only the match outcome.
    cmp_programmed = rd_time() + SSG_AHEAD_TICKS;
    __asm__ volatile("csrw 0x14d, %0" :: "r"(cmp_programmed));
    check(read_stimecmp() == cmp_programmed,
          "stimecmp readback != programmed value");
    uart_puts("arm: stimecmp-readback-match=");
    uart_put_dec(read_stimecmp() == cmp_programmed ? 1UL : 0UL);
    uart_puts(" (expect 1)\n");

    // Gated window: SIE clear, so even though mtime runs past
    // stimecmp and STIP goes pending, no trap may fire.
    gated_window();
    traps_gated = ssg_trap_count;
    uart_puts("gated: reads-done=");
    uart_put_dec(gated_reads_done);
    uart_puts(" sip.STIP-observed=");
    uart_put_dec(ssg_stip_gated);
    uart_puts(" traps-during-window=");
    uart_put_dec(ssg_trap_count);
    uart_puts(" (expect 200000 / 1 / 0)\n");
    check(gated_reads_done == SSG_GATED_READS,
          "gated window did not run to completion");
    check(ssg_stip_gated == 1,
          "sip STIP never went pending during the gated window");
    check(ssg_trap_count == 0, "trap fired with sstatus.SIE clear");
    // Still closed, still pending: the gate holds the trap back.
    check((read_sstatus() & SSTATUS_SIE) == 0, "SIE changed during phase A");

    // Phase B: open the gate inside the labeled wait region. The only
    // SIE=1 transition in the whole run happens there, so the one trap
    // can only land between ssg_loop and ssg_done.
    uart_puts("phase B: opening sstatus.SIE inside the labeled wait loop\n");
    open_gate_and_wait();

    // The one trap: scause must be the supervisor timer interrupt, its
    // sepc must land inside the wait loop, and the counter must read 1.
    uart_puts("trap1: scause=");
    uart_put_hex(ssg_scause);
    uart_puts(" sepc=");
    uart_put_hex(ssg_sepc);
    uart_puts(" traps=");
    uart_put_dec(ssg_trap_count);
    uart_puts("\n");
    uart_puts("wait loop bounds: ssg_loop=");
    uart_put_hex((unsigned long)ssg_loop);
    uart_puts(" ssg_done=");
    uart_put_hex((unsigned long)ssg_done);
    uart_puts(" (diagnostic: sepc varies by a few instructions per run)\n");
    check(ssg_trap_count == 1, "trap never fired after SIE set");
    // sret restores SIE from SPIE, which the trap entry saved as 1, so
    // the gate is open again here.
    sie_after = (read_sstatus() & SSTATUS_SIE) != 0;
    check(sie_after, "SIE not open after the gate-open trap");
    check(ssg_scause == SCAUSE_STI, "scause != supervisor timer interrupt");
    check(ssg_sepc >= (unsigned long)ssg_loop &&
          ssg_sepc < (unsigned long)ssg_done,
          "sepc outside the phase-B wait loop");

    // The in-handler disarm must have dropped the pending source: the
    // comparator reads all-ones and STIP reads clear.
    cmp_final = read_stimecmp();
    sip_final = (read_sip() & SIP_STIP) != 0;
    uart_puts("disarm: stimecmp=");
    uart_put_hex(cmp_final);
    uart_puts(" sip.STIP=");
    uart_put_dec(sip_final);
    uart_puts(" (expect 0xffffffffffffffff / 0)\n");
    check(cmp_final == ~0UL, "handler did not disarm stimecmp");
    check(sip_final == 0, "sip STIP still pending after disarm");

    // Quiet window: the gate is open and the source is disarmed, so
    // nothing may fire. The counter must stay at 1.
    uart_puts("quiet window: ");
    uart_put_dec(SSG_QUIET_READS);
    uart_puts(" rdcycle reads with SIE on, stimecmp=all-ones\n");
    quiet_window();
    traps_quiet = ssg_trap_count - 1; // traps beyond the one gate-open trap
    uart_puts("quiet: traps=");
    uart_put_dec(ssg_trap_count);
    uart_puts(" (expect 1)\n");
    check(ssg_trap_count == 1, "re-delivery fired during the quiet window");

    if (ssg_bad_seen) {
        uart_puts("unexpected extra trap: scause=");
        uart_put_hex(ssg_bad_scause);
        uart_puts(" sepc=");
        uart_put_hex(ssg_bad_sepc);
        uart_puts("\n");
    }

    csum = measured_checksum(traps_gated, traps_quiet, cmp_final,
                             sip_final, sie_after);

    // Verdict section: deterministic measured values only. sepc and the
    // armed stimecmp value are diagnostic above, never part of the
    // verdict or the checksum.
    uart_puts("VERDICT gated_traps=");
    uart_put_dec(traps_gated);
    uart_puts(" gated_stip_observed=");
    uart_put_dec(ssg_stip_gated);
    uart_puts(" gate_open_traps=");
    uart_put_dec(ssg_trap_count);
    uart_puts(" quiet_extra_traps=");
    uart_put_dec(traps_quiet);
    uart_puts(" scause=");
    uart_put_hex(ssg_scause);
    uart_puts(" stimecmp_final=");
    uart_put_hex(cmp_final);
    uart_puts(" stip_final=");
    uart_put_dec(sip_final);
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
    uart_puts("sstatus-sie-gate: S-mode interrupt gate test (backlog item 176)\n");

    // Probe for Sstc: set menvcfg.STCE and check the bit sticks. S-mode
    // access to stimecmp faults without it, so an absent extension fails
    // the run here rather than as a confusing illegal-instruction trap.
    __asm__ volatile("csrs menvcfg, %0" :: "r"(MENVCFG_STCE));
    __asm__ volatile("csrr %0, menvcfg" : "=r"(v));
    ssg_stce = (v & MENVCFG_STCE) != 0;
    uart_puts("Sstc probe: menvcfg.STCE ");
    uart_puts(ssg_stce ? "sticks (present)" : "does not stick (absent)");
    uart_puts("\n");
    check(ssg_stce, "Sstc not present on this hart");

    // Disarm both comparators before any interrupt is enabled. The
    // machine timer stays parked for the whole run; only the supervisor
    // timer is armed later, from S-mode.
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
    __asm__ volatile("csrr %0, mideleg" : "=r"(ssg_mideleg));
    uart_puts("mideleg=");
    uart_put_hex(ssg_mideleg);
    uart_puts("\n");
    check((ssg_mideleg & MIDELEG_STI) != 0, "mideleg STI bit did not stick");

    // M-mode keeps a minimal vector that reports any unexpected M-mode
    // trap and parks; S-mode gets the full vector.
    __asm__ volatile("csrw mtvec, %0" :: "r"(m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(m_scratch_area));
    __asm__ volatile("csrw stvec, %0" :: "r"(ssg_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"(&ssg_save));
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
