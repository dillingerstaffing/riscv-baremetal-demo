// menv_main.c: menvcfg STCE advertisement vs real Sstc presence.
//
// Mechanism under test: menvcfg bit 63 (STCE) is the machine-mode
// advertisement of the Sstc extension. When STCE is 1, S-mode may
// access the stimecmp CSR; when it is 0, S-mode accesses raise an
// illegal-instruction exception. The experiment checks the
// advertisement against reality: it reads menvcfg, probes the bit's
// WARL behavior (clear it, write all-ones, restore the boot value),
// then drops to S-mode and performs a real stimecmp access. The
// advertisement is honest exactly when (STCE == 1) matches (the
// access succeeds). If the access succeeds, the module goes further
// and arms a supervisor timer interrupt from S-mode, requiring
// exactly one trap with scause = 0x8000000000000005 and a quiet
// window with zero re-delivery traps, which is the functional proof
// that Sstc is really there. If the advertisement disagrees with
// reality in either direction, the run fails and the log says which
// direction.
//
// M-mode boot (main):
//   1. Read menvcfg; publish the full 64-bit value and bit 63.
//   2. Clear STCE (csrc), publish the readback; it must equal the
//      boot value with bit 63 clear, proving the clear took effect.
//   3. Write all-ones, publish the legalized readback; the STCE bit
//      must read back set, proving the bit is implemented (WARL).
//   4. Restore the boot value, publish; the readback must equal the
//      boot value.
//   5. Set STCE, publish; record whether the bit sticks.
//   6. Delegate illegal-instruction (bit 2) and the supervisor timer
//      interrupt (bit 5) to S-mode via mideleg; mcounteren = 0x7 so
//      S-mode can use rdtime/rdcycle; one whole-address-space PMP
//      NAPOT entry; install the S-mode and M-mode vectors; mret with
//      MPP=01 into s_main.
// S-mode (s_main):
//   Phase 1: read stimecmp at a labeled 4-byte site. If the access
//   faults, the delegated handler records scause/sepc, advances sepc
//   by 4, and resumes; the destination register must still hold its
//   sentinel. The advertisement check is (STCE==1) == (access ok).
//   Phase 2 (only if the access worked): enable sie.STIE, arm
//   stimecmp = mtime + MENV_AHEAD_TICKS, spin with SIE on until the
//   handler fires. The handler disarms stimecmp to all-ones inside
//   the trap. Then a quiet window of MENV_QUIET_READS rdcycle reads
//   with SIE on must produce zero further traps.
// A 64-bit FNV-1a checksum is fed the verdict-relevant values in a
// fixed order; absolute addresses and timing samples are excluded.
// RESULT: PASS only if every check held. On PASS the virt
// test-device finisher shuts the machine down (QEMU exits 0); on
// FAIL the hart parks in a wfi loop.

#include "menv.h"
#include "../uart.h"

menv_save_t menv_save;
volatile unsigned long menv_trap_count;
volatile unsigned long menv_flag;
volatile unsigned long menv_scause;
volatile unsigned long menv_sepc;
volatile unsigned long menv_sip_after;
volatile unsigned long menv_cmp_after;
volatile unsigned long menv_bad_seen;
volatile unsigned long menv_bad_scause;
volatile unsigned long menv_bad_sepc;
volatile unsigned long menv_arm_cmp;
volatile unsigned long menv_arm_clean;
volatile unsigned long menv_quiet_traps;
volatile unsigned long menv_s_writes;
volatile unsigned long menv_quiet_done;
volatile unsigned long menv_mideleg_rb;
// Recorded by M-mode boot: whether menvcfg.STCE stuck after csrs.
volatile unsigned long menv_stce_present;

static unsigned long m_mscratch_area[4];

static unsigned int checks = 0;
static unsigned int fails = 0;

static unsigned long long cksum = 1469598103934665603ULL;

static void cks_feed(unsigned long v) {
    int i;
    for (i = 0; i < 8; i++) {
        cksum ^= (unsigned long long)((v >> (8 * i)) & 0xffUL);
        cksum *= 1099511628211ULL;
    }
}

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

static unsigned long read_menvcfg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, menvcfg" : "=r"(v));
    return v;
}

static unsigned long read_stimecmp_s(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, 0x14d" : "=r"(v));
    return v;
}

static void write_stimecmp_s(unsigned long v) {
    menv_s_writes++;
    __asm__ volatile("csrw 0x14d, %0" :: "r"(v));
}

static unsigned long read_sip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, sip" : "=r"(v));
    return v;
}

// No M-mode trap is expected after boot. If one fires, print what it
// was and park the hart: the run fails via the timeout harness and the
// log shows the unexpected trap instead of a silent hang.
void menv_m_unexpected_trap(void) {
    unsigned long mcause = m_mscratch_area[1];
    unsigned long mepc = m_mscratch_area[2];
    uart_puts("\nUNEXPECTED M-mode trap: mcause=");
    uart_put_hex(mcause);
    uart_puts(" mepc=");
    uart_put_hex(mepc);
    uart_puts("\nRESULT: FAIL (unexpected M-mode trap)\n");
    for (;;)
        __asm__ volatile("wfi");
}

// S-mode trap handler, on the dedicated trap stack. The first trap
// records scause/sepc; a supervisor timer interrupt additionally
// disarms stimecmp to all-ones inside the handler and records sip and
// the comparator readback. Any further trap is recorded as bad.
// menv_flag wakes the arming spin loop. sepc resume was already
// adjusted at trap entry (+4 for exceptions, unchanged for
// interrupts).
void menv_s_trap_handler(menv_save_t *s) {
    if (menv_trap_count == 0) {
        menv_scause = s->scause;
        menv_sepc = s->sepc;
        if (s->scause == SCAUSE_STI) {
            write_stimecmp_s(~0UL); // disarm inside the handler
            menv_sip_after = read_sip();
            menv_cmp_after = read_stimecmp_s();
        }
    } else {
        menv_bad_seen = 1;
        menv_bad_scause = s->scause;
        menv_bad_sepc = s->sepc;
    }
    menv_trap_count++;
    menv_flag = 1;
}

// The arming spin loop, emitted verbatim: enable SIE, then spin on
// menv_flag (set by the handler) between the global menv_loop /
// menv_done labels, then disable SIE. Marked noinline so the global
// labels are defined exactly once.
__attribute__((noinline)) static void menv_wait(void) {
    menv_flag = 0;
    __asm__ volatile(
        "csrsi sstatus, 2\n"
        ".global menv_loop\n"
        "menv_loop:\n"
        "ld t0, 0(%0)\n"
        "beqz t0, menv_loop\n"
        ".global menv_done\n"
        "menv_done:\n"
        "csrci sstatus, 2\n"
        :
        : "r"(&menv_flag)
        : "t0", "memory");
}

// Arm: write stimecmp = mtime + MENV_AHEAD_TICKS, then record whether
// mtime was still below cmp when the write landed (the clean-arm
// statistic). The trap must fire exactly once; it cannot be missed,
// only already pending.
static void arm_one_shot(void) {
    unsigned long t = rd_time();
    unsigned long cmp = t + MENV_AHEAD_TICKS;
    write_stimecmp_s(cmp);
    menv_arm_cmp = cmp;
    menv_arm_clean = (rd_time() < cmp);
    menv_wait();
}

// Quiet window: SIE on, sie STIE on, stimecmp = all-ones.
// MENV_QUIET_READS rdcycle reads must produce zero traps. The handler
// counts every trap, so menv_trap_count moving catches a re-delivery.
static unsigned long menv_quiet_delta;
static volatile unsigned long menv_sink;

static void quiet_window(void) {
    unsigned long start = rd_cycle();
    unsigned long i;
    __asm__ volatile("csrsi sstatus, 2");
    for (i = 0; i < MENV_QUIET_READS; i++) {
        unsigned long v;
        __asm__ volatile("rdcycle %0" : "=r"(v));
        menv_sink = v;
    }
    __asm__ volatile("csrci sstatus, 2");
    menv_quiet_done = MENV_QUIET_READS;
    menv_quiet_delta = rd_cycle() - start;
}

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U
#define SENTINEL 0xDEADBEEFDEADBEEFUL

// S-mode body. Reached by mret from M-mode boot below.
void s_main(void) {
    unsigned long site, after, v;
    unsigned long cmp_final, before_count;
    int access_ok, ad_match;

    uart_puts("in S-mode: phase 1, stimecmp access probe\n");

    // Phase 1: read stimecmp at a labeled 4-byte site. If Sstc is
    // really available to S-mode this retires silently; otherwise the
    // delegated handler takes an illegal-instruction trap, records
    // scause/sepc, advances sepc by 4, and resumes at label 2.
    menv_trap_count = 0;
    v = SENTINEL;
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "la %1, 2f\n\t"
        "1: csrr %2, 0x14d\n\t"
        "2:\n\t"
        ".option pop\n\t"
        : "=r"(site), "=r"(after), "+r"(v)
        :
        : "memory");

    access_ok = (menv_trap_count == 0);

    uart_puts("probe site (label 1)  = ");
    uart_put_hex(site);
    uart_puts("\nresume site (label 2) = ");
    uart_put_hex(after);
    uart_puts("\nS-mode traps during probe = ");
    uart_put_dec(menv_trap_count);
    uart_puts("\n");
    if (!access_ok) {
        uart_puts("probe scause = ");
        uart_put_hex(menv_scause);
        uart_puts(" sepc = ");
        uart_put_hex(menv_sepc);
        uart_puts(" sepc+4 (resume) = ");
        uart_put_hex(menv_save.sepc_adj);
        uart_puts("\ndest register = ");
        uart_put_hex(v);
        uart_puts(" (sentinel = ");
        uart_put_hex(SENTINEL);
        uart_puts(")\n");
        check(menv_trap_count == 1, "phase 1: probe trap count is not 1");
        check(menv_scause == 2,
              "phase 1: probe scause is not 2 (illegal instruction)");
        check(menv_sepc == site,
              "phase 1: probe sepc is not at the access site");
        check(menv_save.sepc_adj == after,
              "phase 1: sepc+4 is not at the resume label");
        check(v == SENTINEL,
              "phase 1: probe destination changed (it retired?)");
    } else {
        uart_puts("probe readback (stimecmp, observed, not asserted) = ");
        uart_put_hex(v);
        uart_puts("\n");
        check(menv_trap_count == 0,
              "phase 1: unexpected S-mode trap on stimecmp read");
    }
    cks_feed(menv_trap_count);

    // The advertisement check: the STCE bit must agree with reality.
    ad_match = ((menv_stce_present != 0) == (access_ok != 0));
    uart_puts("STCE advertised = ");
    uart_put_dec(menv_stce_present);
    uart_puts(", S-mode access worked = ");
    uart_put_dec((unsigned long)access_ok);
    uart_puts(" -> advertisement ");
    uart_puts(ad_match ? "MATCHES reality\n" : "DISAGREES with reality\n");
    check(ad_match,
          "STCE advertisement disagrees with real Sstc presence");
    cks_feed((unsigned long)ad_match);

    if (!access_ok) {
        // Sstc is not usable from S-mode; the timer arm below would be
        // meaningless, so the run ends here with the checks above.
        uart_puts("stimecmp not accessible from S-mode; "
                  "skipping timer arm\n");
        goto verdict;
    }

    // Phase 2: the functional proof. Arm a supervisor timer interrupt
    // from S-mode; exactly one trap must fire.
    uart_puts("\nphase 2: arming supervisor timer interrupt from S-mode\n");
    __asm__ volatile("csrs sie, %0" :: "r"(SIP_STIP));

    arm_one_shot();
    uart_puts("arm: stimecmp=");
    uart_put_hex(menv_arm_cmp);
    uart_puts(" clean=");
    uart_put_dec(menv_arm_clean);
    uart_puts(" (1 = mtime still below cmp when the write landed)\n");

    uart_puts("trap_count=");
    uart_put_dec(menv_trap_count);
    uart_puts(" scause=");
    uart_put_hex(menv_scause);
    uart_puts(" sepc=");
    uart_put_hex(menv_sepc);
    uart_puts("\nspin loop bounds: menv_loop=");
    uart_put_hex((unsigned long)menv_loop);
    uart_puts(" menv_done=");
    uart_put_hex((unsigned long)menv_done);
    uart_puts("\n");
    uart_puts("sip after in-handler disarm=");
    uart_put_hex(menv_sip_after);
    uart_puts(" (STIP bit ");
    uart_puts((menv_sip_after & SIP_STIP) ? "SET" : "clear");
    uart_puts("), stimecmp readback=");
    uart_put_hex(menv_cmp_after);
    uart_puts("\n");

    check(menv_trap_count == 1, "phase 2: trap count != 1 after arming");
    check(menv_scause == SCAUSE_STI,
          "phase 2: scause != supervisor timer interrupt");
    check(menv_sepc >= (unsigned long)menv_loop &&
          menv_sepc < (unsigned long)menv_done,
          "phase 2: sepc outside the arming spin loop");
    check((menv_sip_after & SIP_STIP) == 0,
          "phase 2: sip STIP still set after disarm write");
    check(menv_cmp_after == ~0UL,
          "phase 2: stimecmp not all-ones after in-handler disarm");
    cks_feed(menv_trap_count);
    cks_feed(menv_scause);

    // Quiet window: disarmed, interrupts on, no trap may fire.
    uart_puts("quiet window: ");
    uart_put_dec(MENV_QUIET_READS);
    uart_puts(" rdcycle reads with SIE on, stimecmp=all-ones\n");
    before_count = menv_trap_count;
    quiet_window();
    menv_quiet_traps = menv_trap_count - before_count;
    uart_puts("quiet window rdcycle delta=");
    uart_put_dec(menv_quiet_delta);
    uart_puts(" (reads done=");
    uart_put_dec(menv_quiet_done);
    uart_puts(")\ntraps in quiet window=");
    uart_put_dec(menv_quiet_traps);
    uart_puts("\n");

    check(menv_quiet_done == MENV_QUIET_READS,
          "quiet window did not run to completion");
    check(menv_quiet_traps == 0, "re-delivery trap fired in quiet window");
    check(menv_trap_count == 1, "trap count != 1 after quiet window");

    cmp_final = read_stimecmp_s();
    uart_puts("stimecmp after quiet window=");
    uart_put_hex(cmp_final);
    uart_puts(" s_writes=");
    uart_put_dec(menv_s_writes);
    uart_puts("\n");
    check(cmp_final == ~0UL, "stimecmp re-armed after the handler disarm");
    check(menv_s_writes == 2, "S-mode stimecmp write count != 2");
    cks_feed(menv_quiet_traps);
    cks_feed(cmp_final);
    cks_feed(menv_s_writes);
    cks_feed(menv_quiet_done);

verdict:
    if (menv_bad_seen) {
        uart_puts("unexpected extra trap: scause=");
        uart_put_hex(menv_bad_scause);
        uart_puts(" sepc=");
        uart_put_hex(menv_bad_sepc);
        uart_puts("\n");
    }

    uart_puts("\nchecksum (FNV-1a over verdict values) = ");
    {
        unsigned long long h = cksum;
        int i;
        uart_puts("0x");
        for (i = 15; i >= 0; i--) {
            unsigned int d = (unsigned int)((h >> (4 * i)) & 0xfULL);
            uart_putc(d < 10 ? (char)('0' + d) : (char)('a' + d - 10));
        }
    }
    uart_puts("\nchecks=");
    uart_put_dec(checks);
    uart_puts(" fails=");
    uart_put_dec(fails);
    uart_puts("\n");
    uart_puts(fails == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");

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
    // harness runs QEMU under timeout, so a FAIL is observable as the
    // timeout exit status (124) as well as the RESULT line.
    for (;;)
        __asm__ volatile("wfi");
}

// M-mode boot. boot.S jumps here in M-mode.
int main(void) {
    unsigned long boot, r1, r2, r3, r4, rb, v;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("menvcfg-stce: STCE advertisement vs real Sstc presence\n");
    uart_puts("========================================\n\n");

    // 1. Boot menvcfg and the STCE bit.
    boot = read_menvcfg();
    uart_puts("boot: menvcfg=");
    uart_put_hex(boot);
    uart_puts(" STCE(bit63)=");
    uart_put_dec((boot >> 63) & 1UL);
    uart_puts("\n");
    cks_feed(boot);

    // 2. Clear probe: csrc STCE, read back. The readback must equal
    // the boot value with bit 63 clear, proving the clear took effect
    // rather than being silently ignored.
    __asm__ volatile("csrc menvcfg, %0" :: "r"(MENVCFG_STCE));
    r1 = read_menvcfg();
    uart_puts("probe: csrc menvcfg, STCE; readback=");
    uart_put_hex(r1);
    uart_puts("\n");
    check(r1 == (boot & ~MENVCFG_STCE),
          "menvcfg STCE clear did not read back cleared");
    cks_feed(r1);

    // 3. All-ones probe: write all-ones, publish the legalized
    // readback. The STCE bit must read back set, proving the bit is
    // implemented (WARL) on this hart.
    __asm__ volatile("csrw menvcfg, %0" :: "r"(~0UL));
    r2 = read_menvcfg();
    uart_puts("probe: csrw menvcfg, all-ones; legalized readback=");
    uart_put_hex(r2);
    uart_puts("\n");
    check((r2 & MENVCFG_STCE) == MENVCFG_STCE,
          "menvcfg STCE bit did not stick on all-ones write");
    cks_feed(r2);

    // 4. Restore the boot value; the readback must equal it.
    __asm__ volatile("csrw menvcfg, %0" :: "r"(boot));
    r3 = read_menvcfg();
    uart_puts("restore: csrw menvcfg, boot value; readback=");
    uart_put_hex(r3);
    uart_puts("\n");
    check(r3 == boot, "menvcfg restore did not read back the boot value");
    cks_feed(r3);

    // 5. Set STCE and record whether the bit sticks: this is the
    // advertisement the S-mode half of the experiment tests.
    __asm__ volatile("csrs menvcfg, %0" :: "r"(MENVCFG_STCE));
    r4 = read_menvcfg();
    menv_stce_present = ((r4 & MENVCFG_STCE) != 0);
    uart_puts("set: csrs menvcfg, STCE; readback=");
    uart_put_hex(r4);
    uart_puts(" STCE sticks: ");
    uart_puts(menv_stce_present ? "yes (Sstc advertised)\n"
                                : "no (Sstc not advertised)\n");
    cks_feed(r4);

    // 6. Delegate illegal-instruction (bit 2) and the supervisor
    // timer interrupt (bit 5) to S-mode.
    __asm__ volatile("csrw mideleg, %0" :: "r"(MENV_MIDELEG));
    __asm__ volatile("csrr %0, mideleg" : "=r"(rb));
    menv_mideleg_rb = rb;
    uart_puts("mideleg readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check((rb & MENV_MIDELEG) == MENV_MIDELEG,
          "mideleg bits 2 and 5 did not stick");
    cks_feed(rb & MENV_MIDELEG);

    // mcounteren: S-mode needs rdtime (arming) and rdcycle (quiet
    // window).
    __asm__ volatile("csrw mcounteren, %0" :: "r"(0x7UL));

    // PMP: with no PMP entry programmed, S-mode has no access to any
    // address. Open the whole address space with one NAPOT R/W/X
    // entry before the drop.
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t" // A=NAPOT, R/W/X, unlocked
                     "csrw pmpcfg0, t0\n\t"
                     :
                     :
                     : "t0", "memory");

    // Vectors: M-mode parks on any unexpected trap; S-mode gets the
    // full vector.
    __asm__ volatile("la t0, menv_mtrap_entry\n\t"
                     "csrw mtvec, t0\n\t"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("csrw mscratch, %0" :: "r"(m_mscratch_area));
    __asm__ volatile("la t0, menv_strap_entry\n\t"
                     "csrw stvec, t0\n\t"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("la t0, menv_save\n\t"
                     "csrw sscratch, t0\n\t"
                     :
                     :
                     : "t0", "memory");
    __asm__ volatile("csrr %0, stvec" : "=r"(v));
    check((v & 3UL) == 0, "stvec not in direct mode");

    uart_puts("setup complete; dropping to S-mode...\n");

    // mret with MPP=01 (S-mode) into s_main.
    __asm__ volatile("la t0, s_main\n\t"
                     "csrw mepc, t0\n\t"
                     "csrr t0, mstatus\n\t"
                     "li t1, 0x1800\n\t"   // clear MPP bits 12:11
                     "not t1, t1\n\t"
                     "and t0, t0, t1\n\t"
                     "li t1, 0x800\n\t"    // MPP = 01 (S-mode)
                     "or t0, t0, t1\n\t"
                     "csrw mstatus, t0\n\t"
                     "mret\n\t"
                     :
                     :
                     : "t0", "t1", "memory");

    // Unreachable: mret lands in the S-mode payload, which finishes
    // via the test-device finisher or parks on failure.
    for (;;)
        __asm__ volatile("wfi");
}
