// stg_main.c: sie.STIE as the S-mode per-interrupt enable gate.
//
// The behavior under test: sie.STIE is the S-mode enable bit
// for the supervisor timer interrupt, independent of the global
// sstatus.SIE gate. A pended STIP must stay pending without trapping
// while STIE is clear (even with SIE set), and must trap exactly once
// STIE is set.
//
// Phase A (gate closed, runs in S-mode with SIE SET, STIE CLEAR):
//   1. Global sstatus.SIE is opened explicitly; the gate under test,
//      sie.STIE, starts CLEAR.
//   2. Arm stimecmp = mtime + STG_AHEAD_TICKS. Poll a bounded window
//      of STG_GATED_READS rdcycle reads. mtime runs past stimecmp
//      inside the window, so sip STIP must read 1 (the interrupt is
//      pending) while the trap counter stays at 0 (the gate is closed).
// Phase B (gate open):
//   3. csrs sie, t0 with t0 = SIE_STIE (bit 5; not encodable in the
//      5-bit csrsi immediate, hence csrs via a register). The pending
//      interrupt must trap exactly once. The handler records
//      scause (expect 0x8000000000000005, the supervisor timer
//      interrupt) and sepc, then disarms stimecmp to all-ones so the
//      level-triggered source drops and no re-delivery can occur.
//   4. Quiet window with STIE still on: STG_QUIET_READS rdcycle reads;
//      the trap counter must stay at 1, sip STIP must read 0, and
//      stimecmp must still read all-ones.
//
// All waits are bounded by instruction or mtime budgets, never open
// ended, so a broken machine yields FAIL lines, not a hang. The
// verdict-relevant output lines carry register values and small counts
// only; host-timing-dependent values (the armed stimecmp, sepc) are
// printed as match booleans or marked diagnostic. A 64-bit FNV-1a
// checksum over the deterministic measured values is printed and
// cross-checked in the PROOF.md results table.

#include "stg.h"
#include "../uart.h"

stg_save_t stg_save;
volatile unsigned long stg_trap_count;
volatile unsigned long stg_scause;
volatile unsigned long stg_sepc;
volatile unsigned long stg_stip_gated;
volatile unsigned long stg_bad_seen;
volatile unsigned long stg_bad_scause;
volatile unsigned long stg_bad_sepc;
volatile unsigned long stg_stce;
volatile unsigned long stg_mideleg;

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
void stg_trap_handler(stg_save_t *s) {
    if (stg_trap_count == 0) {
        stg_scause = s->scause;
        stg_sepc = s->sepc;
        __asm__ volatile("csrw 0x14d, %0" :: "r"(~0UL)); // disarm
    } else {
        stg_bad_seen = 1;
        stg_bad_scause = s->scause;
        stg_bad_sepc = s->sepc;
    }
    stg_trap_count++;
}

// Phase-A gated window, emitted verbatim: with SIE known set and STIE
// known clear, spin STG_GATED_READS rdcycle reads, watching sip STIP.
// The volatile sink keeps the loop from being folded away. Marked
// noinline so the window is emitted exactly once; the trap counter
// must not move here.
static volatile unsigned long stg_sink;
__attribute__((noinline)) static void gated_window(void) {
    unsigned long i;
    for (i = 0; i < STG_GATED_READS; i++) {
        unsigned long v, sip;
        __asm__ volatile("rdcycle %0" : "=r"(v));
        __asm__ volatile("csrr %0, sip" : "=r"(sip));
        if (sip & SIP_STIP)
            stg_stip_gated = 1;
        stg_sink = v;
    }
    gated_reads_done = STG_GATED_READS;
}

// Phase-B wait loop, emitted verbatim: the gate opens exactly at the
// csrs inside the labeled region, then spin on stg_trap_count with a
// decrementing budget until the pending trap lands. Because the
// interrupt was already pending before the gate opens, the trap lands
// at an instruction boundary inside the stg_loop/stg_done region.
// Marked noinline so the labels are defined exactly once; sepc of the
// one trap must land between them. (csrsi cannot set bit 5: its
// immediate is 5 bits wide, so the set goes through a register.)
__attribute__((noinline)) static void open_gate_and_wait(void) {
    __asm__ volatile(
        ".global stg_loop\n"
        "stg_loop:\n"
        "li t0, 0x20\n"
        "csrs sie, t0\n"        // open the gate: the pending trap can land here
        "mv t1, %1\n"
        "1:\n"
        "ld t0, 0(%0)\n"
        "bnez t0, 2f\n"
        "addi t1, t1, -1\n"
        "bnez t1, 1b\n"
        "2:\n"
        ".global stg_done\n"
        "stg_done:\n"
        :
        : "r"(&stg_trap_count), "r"(STG_TRAP_WAIT_READS)
        : "t0", "t1", "memory");
}

// Quiet window: SIE on, sie STIE on, stimecmp disarmed. STG_QUIET_READS
// rdcycle reads must produce zero traps. Marked noinline like the
// other windows.
__attribute__((noinline)) static void quiet_window(void) {
    unsigned long i;
    for (i = 0; i < STG_QUIET_READS; i++) {
        unsigned long v;
        __asm__ volatile("rdcycle %0" : "=r"(v));
        stg_sink = v;
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
// the STIE bit readback after the release. sepc and the armed stimecmp
// value are excluded: both are diagnostic and host-timing dependent,
// so they must not enter the checksum.
static unsigned long measured_checksum(unsigned long traps_gated,
                                       unsigned long traps_quiet,
                                       unsigned long cmp_final,
                                       unsigned long sip_final,
                                       unsigned long stie_after) {
    unsigned long h = 0xcbf29ce484222325UL;
    h = fnv1a_64(h, traps_gated);
    h = fnv1a_64(h, stg_stip_gated);
    h = fnv1a_64(h, stg_trap_count);
    h = fnv1a_64(h, stg_scause);
    h = fnv1a_64(h, cmp_final);
    h = fnv1a_64(h, sip_final);
    h = fnv1a_64(h, traps_quiet);
    h = fnv1a_64(h, stie_after);
    return h;
}

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U

// S-mode body. Reached by mret from M-mode boot below.
void s_main(void) {
    unsigned long cmp_boot, cmp_programmed, cmp_final, sip_final;
    unsigned long stie_after, traps_gated, traps_quiet, csum;

    uart_puts("in S-mode: opening global sstatus.SIE, keeping sie.STIE clear\n");

    // Open the global gate first: the experiment shows STIE alone is
    // the gate, so SIE must not be the thing blocking delivery.
    __asm__ volatile("csrs sstatus, %0" :: "r"(SSTATUS_SIE));
    check((read_sstatus() & SSTATUS_SIE) != 0, "SIE not set after enable");

    // The gate under test starts clear; the M-mode boot never sets it.
    check((read_sie() & SIE_STIE) == 0, "sie.STIE not clear at phase A");

    // The M-mode boot disarm must be visible here: stimecmp reads
    // all-ones before S-mode ever touches it.
    cmp_boot = read_stimecmp();
    uart_puts("stimecmp at S-mode entry=");
    uart_put_hex(cmp_boot);
    uart_puts("\n");
    check(cmp_boot == ~0UL, "stimecmp not all-ones at S-mode entry");

    uart_puts("phase A: arming stimecmp = mtime+");
    uart_put_dec(STG_AHEAD_TICKS);
    uart_puts(" ticks with STIE clear\n");

    // Arm the timer. The programmed value depends on boot-time mtime,
    // so the printed line reports only the match outcome.
    cmp_programmed = rd_time() + STG_AHEAD_TICKS;
    __asm__ volatile("csrw 0x14d, %0" :: "r"(cmp_programmed));
    check(read_stimecmp() == cmp_programmed,
          "stimecmp readback != programmed value");
    uart_puts("arm: stimecmp-readback-match=");
    uart_put_dec(read_stimecmp() == cmp_programmed ? 1UL : 0UL);
    uart_puts(" (expect 1)\n");

    // Gated window: SIE is set, so the only thing between the pending
    // interrupt and the hart is STIE=0. mtime runs past stimecmp,
    // STIP goes pending, and no trap may fire.
    gated_window();
    traps_gated = stg_trap_count;
    uart_puts("gated: reads-done=");
    uart_put_dec(gated_reads_done);
    uart_puts(" sip.STIP-observed=");
    uart_put_dec(stg_stip_gated);
    uart_puts(" traps-during-window=");
    uart_put_dec(stg_trap_count);
    uart_puts(" (expect 100000 / 1 / 0)\n");
    check(gated_reads_done == STG_GATED_READS,
          "gated window did not run to completion");
    check(stg_stip_gated == 1,
          "sip STIP never went pending during the gated window");
    check(stg_trap_count == 0, "trap fired with sie.STIE clear");
    // Still closed, still pending: STIE holds the trap back.
    check((read_sie() & SIE_STIE) == 0, "sie.STIE changed during phase A");

    // Phase B: open the gate inside the labeled wait region. The only
    // STIE 0-to-1 transition in the whole run happens there, so the one
    // trap can only land between stg_loop and stg_done.
    uart_puts("phase B: opening sie.STIE inside the labeled wait loop\n");
    open_gate_and_wait();

    // The one trap: scause must be the supervisor timer interrupt, its
    // sepc must land inside the wait loop, and the counter must read 1.
    uart_puts("trap1: scause=");
    uart_put_hex(stg_scause);
    uart_puts(" sepc=");
    uart_put_hex(stg_sepc);
    uart_puts(" traps=");
    uart_put_dec(stg_trap_count);
    uart_puts("\n");
    uart_puts("wait loop bounds: stg_loop=");
    uart_put_hex((unsigned long)stg_loop);
    uart_puts(" stg_done=");
    uart_put_hex((unsigned long)stg_done);
    uart_puts(" (diagnostic: sepc varies by a few instructions per run)\n");
    check(stg_trap_count == 1, "trap never fired after STIE set");
    // The gate is open now; read back the bit that opened it.
    stie_after = (read_sie() & SIE_STIE) != 0;
    check(stie_after, "sie.STIE not set after the gate-open");
    check(stg_scause == SCAUSE_STI, "scause != supervisor timer interrupt");
    check(stg_sepc >= (unsigned long)stg_loop &&
          stg_sepc < (unsigned long)stg_done,
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

    // Quiet window: both gates are open and the source is disarmed, so
    // nothing may fire. The counter must stay at 1.
    uart_puts("quiet window: ");
    uart_put_dec(STG_QUIET_READS);
    uart_puts(" rdcycle reads with STIE on, stimecmp=all-ones\n");
    quiet_window();
    traps_quiet = stg_trap_count - 1; // traps beyond the one gate-open trap
    uart_puts("quiet: traps=");
    uart_put_dec(stg_trap_count);
    uart_puts(" (expect 1)\n");
    check(stg_trap_count == 1, "re-delivery fired during the quiet window");

    if (stg_bad_seen) {
        uart_puts("unexpected extra trap: scause=");
        uart_put_hex(stg_bad_scause);
        uart_puts(" sepc=");
        uart_put_hex(stg_bad_sepc);
        uart_puts("\n");
    }

    csum = measured_checksum(traps_gated, traps_quiet, cmp_final,
                             sip_final, stie_after);

    // Verdict section: deterministic measured values only. sepc and the
    // armed stimecmp value are diagnostic above, never part of the
    // verdict or the checksum.
    uart_puts("VERDICT gated_traps=");
    uart_put_dec(traps_gated);
    uart_puts(" gated_stip_observed=");
    uart_put_dec(stg_stip_gated);
    uart_puts(" gate_open_traps=");
    uart_put_dec(stg_trap_count);
    uart_puts(" quiet_extra_traps=");
    uart_put_dec(traps_quiet);
    uart_puts(" scause=");
    uart_put_hex(stg_scause);
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
    uart_puts("sie-stie-gate: S-mode per-interrupt enable test\n");

    // Probe for Sstc: set menvcfg.STCE and check the bit sticks. S-mode
    // access to stimecmp faults without it, so an absent extension fails
    // the run here rather than as a confusing illegal-instruction trap.
    __asm__ volatile("csrs menvcfg, %0" :: "r"(MENVCFG_STCE));
    __asm__ volatile("csrr %0, menvcfg" : "=r"(v));
    stg_stce = (v & MENVCFG_STCE) != 0;
    uart_puts("Sstc probe: menvcfg.STCE ");
    uart_puts(stg_stce ? "sticks (present)" : "does not stick (absent)");
    uart_puts("\n");
    check(stg_stce, "Sstc not present on this hart");

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
    __asm__ volatile("csrr %0, mideleg" : "=r"(stg_mideleg));
    uart_puts("mideleg=");
    uart_put_hex(stg_mideleg);
    uart_puts("\n");
    check((stg_mideleg & MIDELEG_STI) != 0, "mideleg STI bit did not stick");

    // M-mode keeps a minimal vector that reports any unexpected M-mode
    // trap and parks; S-mode gets the full vector.
    __asm__ volatile("csrw mtvec, %0" :: "r"(m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(m_scratch_area));
    __asm__ volatile("csrw stvec, %0" :: "r"(stg_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"(&stg_save));
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
