// stos_main.c: stimecmp one-shot disarm experiment (proof backlog item 137).
//
// Fundamental truth under test: a supervisor timer interrupt is a one-shot
// event once stimecmp is disarmed inside the S-mode handler. The pending
// state clears and no new trap fires until a future stimecmp is armed.
//
// M-mode boot (main): probe the Sstc extension via menvcfg.STCE, disarm the
// machine timer (CLINT mtimecmp = all-ones) and stimecmp (all-ones) once,
// delegate the supervisor timer interrupt to S-mode via mideleg bit 5,
// grant S-mode access to the cycle/time counters via mcounteren, open the
// address space to S-mode with a PMP NAPOT entry, install the S-mode and
// M-mode trap vectors, and mret into S-mode. M-mode never writes stimecmp
// again: the write counter stos_m_writes must read exactly 1 at the end.
//
// S-mode (s_main): enable the supervisor timer interrupt in sie, arm
// stimecmp 1000 mtime ticks ahead, take the one trap (scause must be
// 0x8000000000000005), and inside the handler write stimecmp to all-ones
// (disarm), confirm the sip STIP bit dropped, and sret. Then, with sstatus
// SIE and sie STIE still enabled, the hart spins through a quiet window
// of STOS_QUIET_READS (1,000,000) rdcycle reads; the C handler counts every
// trap, so any re-delivery during the window is caught and fails the run.
//
// The verdict is PASS only if all of: exactly one trap fired, its scause
// is the supervisor timer interrupt, its sepc lies inside the arming spin
// loop (between stos_loop and stos_done), the sip STIP bit reads clear
// after the in-handler disarm write, the quiet window shows zero
// additional traps, stimecmp still reads all-ones after the quiet window,
// and the stimecmp write counters read exactly 1 (M-mode, the boot disarm)
// and 2 (S-mode, the arm write plus the in-handler disarm write).
//
// The spin loop's trap and resume addresses are in-asm global labels;
// no C labels-as-values are used (distro gcc miscompiles &&label at
// -O2, documented in this project's memory).

#include "stos.h"
#include "../uart.h"

stos_save_t stos_save;
volatile unsigned long stos_trap_count;
volatile unsigned long stos_flag;
volatile unsigned long stos_scause;
volatile unsigned long stos_sepc;
volatile unsigned long stos_sip_after_disarm;
volatile unsigned long stos_cmp_after_disarm;
volatile unsigned long stos_bad_seen;
volatile unsigned long stos_bad_scause;
volatile unsigned long stos_bad_sepc;
volatile unsigned long stos_arm_cmp;
volatile unsigned long stos_arm_clean;
volatile unsigned long stos_quiet_traps;
volatile unsigned long stos_m_writes;
volatile unsigned long stos_s_writes;
volatile unsigned long stos_mideleg;
volatile unsigned long stos_stce;
volatile unsigned long stos_quiet_reads_done;

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

static unsigned long rd_cycle(void) {
    unsigned long v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
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

// M-mode stimecmp write. Used exactly once, at boot, to disarm.
static void write_stimecmp_m(unsigned long v) {
    stos_m_writes++;
    __asm__ volatile("csrw 0x14d, %0" :: "r"(v));
}

// S-mode stimecmp write. Used exactly twice: the arm write, then the
// in-handler disarm write.
static void write_stimecmp_s(unsigned long v) {
    stos_s_writes++;
    __asm__ volatile("csrw 0x14d, %0" :: "r"(v));
}

static unsigned long read_sip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, sip" : "=r"(v));
    return v;
}

extern void m_trap_entry(void);

// No M-mode trap is expected after boot. If one fires, print what it was
// and park the hart: the run fails via the timeout harness and the log
// shows the unexpected trap instead of a silent hang.
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

// S-mode trap handler: runs on the dedicated trap stack with all registers
// saved. The first trap records scause/sepc, disarms the timer inside the
// handler (stimecmp = all-ones), and confirms the sip STIP bit dropped and
// the comparator reads back all-ones. Any further trap (there must be none)
// is recorded as bad. stos_flag wakes the spin loop; sret resumes the
// interrupted code.
void stos_trap_handler(stos_save_t *s) {
    if (stos_trap_count == 0) {
        stos_scause = s->scause;
        stos_sepc = s->sepc;
        write_stimecmp_s(~0UL);   // disarm inside the handler
        stos_sip_after_disarm = read_sip();
        stos_cmp_after_disarm = read_stimecmp();
    } else {
        stos_bad_seen = 1;
        stos_bad_scause = s->scause;
        stos_bad_sepc = s->sepc;
    }
    stos_trap_count++;
    stos_flag = 1;
}

// The arming spin loop, emitted verbatim: enable SIE, then spin on
// stos_flag (set by the handler) between the global stos_loop/stos_done
// labels, then disable SIE. Marked noinline so the global labels are
// defined exactly once.
__attribute__((noinline)) static void stos_wait(void) {
    stos_flag = 0;
    __asm__ volatile(
        "csrsi sstatus, 2\n"
        ".global stos_loop\n"
        "stos_loop:\n"
        "ld t0, 0(%0)\n"
        "beqz t0, stos_loop\n"
        ".global stos_done\n"
        "stos_done:\n"
        "csrci sstatus, 2\n"
        :
        : "r"(&stos_flag)
        : "t0", "memory");
}

// Arm: write stimecmp = mtime + STOS_AHEAD_TICKS (1000 ticks ahead), then
// record whether mtime was still below cmp when the write landed (the
// clean-arm statistic). With 1000 ticks (100 us at the 10 MHz timebase)
// the write always lands ahead of the deadline; the statistic documents
// that rather than assuming it. The trap must fire exactly once; it
// cannot be missed, only already pending.
static void arm_one_shot(void) {
    unsigned long t = rd_time();
    unsigned long cmp = t + STOS_AHEAD_TICKS;
    write_stimecmp_s(cmp);
    stos_arm_cmp = cmp;
    stos_arm_clean = (rd_time() < cmp);
    stos_wait();
}

// Quiet window: SIE on, sie STIE on, stimecmp = all-ones. STOS_QUIET_READS
// rdcycle reads must produce zero traps. The handler counts any trap, so
// stos_trap_count moving catches a re-delivery. The volatile sink keeps
// the reads from being folded away; the before/after rdcycle delta proves
// the window ran.
static unsigned long stos_quiet_delta;
static volatile unsigned long stos_sink;

static void quiet_window(void) {
    unsigned long start = rd_cycle();
    unsigned long i;
    __asm__ volatile("csrsi sstatus, 2");
    for (i = 0; i < STOS_QUIET_READS; i++) {
        unsigned long v;
        __asm__ volatile("rdcycle %0" : "=r"(v));
        stos_sink = v;
    }
    __asm__ volatile("csrci sstatus, 2");
    stos_quiet_reads_done = STOS_QUIET_READS;
    stos_quiet_delta = rd_cycle() - start;
}

// FNV-1a 64-bit over the deterministic measured values. The hashed fields
// are: stos_trap_count, stos_scause, stos_sepc, stos_quiet_traps, the
// post-quiet-window stimecmp readback, stos_m_writes, stos_s_writes, and
// stos_quiet_reads_done, each as one 64-bit word in that order.
static unsigned long fnv1a_64(unsigned long h, unsigned long v) {
    h ^= v;
    h *= 0x100000001b3UL;
    return h;
}

static unsigned long measured_checksum(unsigned long cmp_final) {
    unsigned long h = 0xcbf29ce484222325UL;
    h = fnv1a_64(h, stos_trap_count);
    h = fnv1a_64(h, stos_scause);
    h = fnv1a_64(h, stos_sepc);
    h = fnv1a_64(h, stos_quiet_traps);
    h = fnv1a_64(h, cmp_final);
    h = fnv1a_64(h, stos_m_writes);
    h = fnv1a_64(h, stos_s_writes);
    h = fnv1a_64(h, stos_quiet_reads_done);
    return h;
}

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U

// S-mode body. Reached by mret from M-mode boot below.
void s_main(void) {
    unsigned long cmp_boot, sip_before, cmp_final, csum, before_count;

    uart_puts("in S-mode: enabling sie.STIE, checking boot disarm\n");

    // The M-mode boot disarm must be visible here: stimecmp reads
    // all-ones before S-mode ever touches it.
    cmp_boot = read_stimecmp();
    uart_puts("stimecmp at S-mode entry=");
    uart_put_hex(cmp_boot);
    uart_puts("\n");
    check(cmp_boot == ~0UL, "stimecmp not all-ones at S-mode entry");

    sip_before = read_sip();
    uart_puts("sip before arming=");
    uart_put_hex(sip_before);
    uart_puts("\n");
    check((sip_before & SIP_STIP) == 0, "sip STIP set before arming");

    __asm__ volatile("csrs sie, %0" :: "r"(SIP_STIP));

    uart_puts("arming stimecmp = mtime+");
    uart_put_dec(STOS_AHEAD_TICKS);
    uart_puts(" ticks\n");

    arm_one_shot();
    uart_puts("arm: stimecmp=");
    uart_put_hex(stos_arm_cmp);
    uart_puts(" clean=");
    uart_put_dec(stos_arm_clean);
    uart_puts(" (1 = mtime still below cmp when the write landed)\n");

    // The single trap, before the quiet window.
    uart_puts("trap_count=");
    uart_put_dec(stos_trap_count);
    uart_puts(" scause=");
    uart_put_hex(stos_scause);
    uart_puts(" sepc=");
    uart_put_hex(stos_sepc);
    uart_puts("\n");
    uart_puts("spin loop bounds: stos_loop=");
    uart_put_hex((unsigned long)stos_loop);
    uart_puts(" stos_done=");
    uart_put_hex((unsigned long)stos_done);
    uart_puts("\n");
    uart_puts("sip after in-handler disarm=");
    uart_put_hex(stos_sip_after_disarm);
    uart_puts(" (STIP bit ");
    uart_puts((stos_sip_after_disarm & SIP_STIP) ? "SET" : "clear");
    uart_puts("), stimecmp readback=");
    uart_put_hex(stos_cmp_after_disarm);
    uart_puts("\n");

    check(stos_trap_count == 1, "trap count != 1 after arming");
    check(stos_scause == SCAUSE_STI, "scause != supervisor timer interrupt");
    check(stos_sepc >= (unsigned long)stos_loop &&
          stos_sepc < (unsigned long)stos_done,
          "sepc outside the arming spin loop");
    check((stos_sip_after_disarm & SIP_STIP) == 0,
          "sip STIP still set after disarm write");
    check(stos_cmp_after_disarm == ~0UL,
          "stimecmp not all-ones after in-handler disarm");

    // Quiet window: disarmed, interrupts on, no trap may fire.
    uart_puts("quiet window: ");
    uart_put_dec(STOS_QUIET_READS);
    uart_puts(" rdcycle reads with SIE on, stimecmp=all-ones\n");
    before_count = stos_trap_count;
    quiet_window();
    stos_quiet_traps = stos_trap_count - before_count;
    uart_puts("quiet window rdcycle delta=");
    uart_put_dec(stos_quiet_delta);
    uart_puts(" (reads done=");
    uart_put_dec(stos_quiet_reads_done);
    uart_puts(")\n");
    uart_puts("traps in quiet window=");
    uart_put_dec(stos_quiet_traps);
    uart_puts("\n");

    check(stos_quiet_reads_done == STOS_QUIET_READS,
          "quiet window did not run to completion");
    check(stos_quiet_traps == 0, "re-delivery trap fired in quiet window");
    check(stos_trap_count == 1, "trap count != 1 after quiet window");

    // Nobody re-armed behind our back: stimecmp must still read all-ones
    // after the quiet window, and the write counters must show exactly
    // the three writes this program makes: one M-mode boot disarm, the
    // S-mode arm write, and the S-mode in-handler disarm write.
    cmp_final = read_stimecmp();
    uart_puts("stimecmp after quiet window=");
    uart_put_hex(cmp_final);
    uart_puts(" m_writes=");
    uart_put_dec(stos_m_writes);
    uart_puts(" s_writes=");
    uart_put_dec(stos_s_writes);
    uart_puts("\n");

    check(cmp_final == ~0UL, "stimecmp re-armed after the handler disarm");
    check(stos_m_writes == 1, "M-mode wrote stimecmp more than once");
    check(stos_s_writes == 2, "S-mode stimecmp write count != 2");

    if (stos_bad_seen) {
        uart_puts("unexpected extra trap: scause=");
        uart_put_hex(stos_bad_scause);
        uart_puts(" sepc=");
        uart_put_hex(stos_bad_sepc);
        uart_puts("\n");
    }

    csum = measured_checksum(cmp_final);

    // Verdict section: the deterministic measured values.
    uart_puts("VERDICT trap_count=");
    uart_put_dec(stos_trap_count);
    uart_puts(" scause=");
    uart_put_hex(stos_scause);
    uart_puts(" sepc=");
    uart_put_hex(stos_sepc);
    uart_puts(" quiet_window_reads=");
    uart_put_dec(STOS_QUIET_READS);
    uart_puts(" quiet_window_traps=");
    uart_put_dec(stos_quiet_traps);
    uart_puts(" stimecmp_final=");
    uart_put_hex(cmp_final);
    uart_puts(" m_writes=");
    uart_put_dec(stos_m_writes);
    uart_puts(" s_writes=");
    uart_put_dec(stos_s_writes);
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
    uart_puts("stimecmp-one-shot: S-mode one-shot disarm, quiet window (backlog item 137)\n");

    // Probe for Sstc: set menvcfg.STCE and check the bit sticks. S-mode
    // access to stimecmp faults without it, so an absent extension fails
    // the run here rather than as a confusing illegal-instruction trap.
    __asm__ volatile("csrs menvcfg, %0" :: "r"(MENVCFG_STCE));
    __asm__ volatile("csrr %0, menvcfg" : "=r"(v));
    stos_stce = (v & MENVCFG_STCE) != 0;
    uart_puts("Sstc probe: menvcfg.STCE ");
    uart_puts(stos_stce ? "sticks (present)" : "does not stick (absent)");
    uart_puts("\n");
    check(stos_stce, "Sstc not present on this hart");

    // Disarm both comparators before any interrupt is enabled. The
    // machine timer stays parked for the whole run; only the supervisor
    // timer is armed later, from S-mode. This M-mode write is the only
    // stimecmp write M-mode ever performs.
    *(volatile unsigned long *)CLINT_MTIMECMP = ~0UL;
    write_stimecmp_m(~0UL);

    // PMP: with no PMP entry programmed, S-mode has no access to any
    // address (M-mode keeps full access, lower modes default-deny).
    // Open the whole address space to S-mode with one NAPOT entry,
    // R/W/X, before the drop.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // mcounteren: time/cycle/instret are unreadable in S-mode unless
    // M-mode grants access; the S-mode arming code uses rdtime and the
    // quiet window uses rdcycle.
    __asm__ volatile("csrw mcounteren, %0" :: "r"(0x7UL));  // CY|TM|IR

    // Delegate only the supervisor timer interrupt to S-mode.
    __asm__ volatile("csrw mideleg, %0" :: "r"(MIDELEG_STI));
    __asm__ volatile("csrr %0, mideleg" : "=r"(stos_mideleg));
    uart_puts("mideleg=");
    uart_put_hex(stos_mideleg);
    uart_puts("\n");
    check((stos_mideleg & MIDELEG_STI) != 0, "mideleg STI bit did not stick");

    // M-mode keeps a minimal vector that reports any unexpected M-mode
    // trap and parks; S-mode gets the full vector.
    __asm__ volatile("csrw mtvec, %0" :: "r"(m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(m_scratch_area));
    __asm__ volatile("csrw stvec, %0" :: "r"(stos_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"(&stos_save));
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
