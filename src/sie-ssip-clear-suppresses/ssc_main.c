// ssc_main.c: sie.SSIE as the S-mode per-interrupt enable gate
// (backlog item "riscv sie-ssip-clear-suppresses").
//
// Exactly one mechanism is under test: sie.SSIE is the
// supervisor-mode enable bit for the supervisor software interrupt,
// independent of the global sstatus.SIE gate. A pended SSIP must
// stay pending without trapping while SSIE is clear (even with SIE
// set), and must trap exactly once when SSIE is set.
//
// Sequence:
//   M-mode: record the boot mideleg value, write bit 1 and require
//   the readback to show bit 1 set with bit 9 clear (only the SSI is
//   delegated), write 0 to medeleg and require the readback to be 0,
//   install direct-mode mtvec/mscratch and stvec/sscratch, open the
//   whole address space to S-mode with one PMP NAPOT entry, grant
//   S-mode rdcycle/rdtime via mcounteren, clear mie and
//   mstatus.MIE, pend SSIP from M-mode with csrs mip (S-mode sip
//   writes to bit 1 are dropped on this hart, so the pend happens
//   before the drop), clear sie entirely (the gate under test starts
//   closed), then sret to S-mode with sstatus.SIE set.
//   S-mode phase 1 (gate closed): sstatus.SIE is set, sie.SSIE is
//   clear. Poll a bounded window of SSC_GATED_READS iterations,
//   watching sip; SSIP must read 1 throughout while the trap count
//   stays 0.
//   S-mode phase 2 (gate open): set sie.SSIE inside a labeled wait
//   loop. The pending SSI must trap exactly once, at an instruction
//   inside the loop, with scause = 0x8000000000000001. The handler
//   records scause/sepc and the sip value at entry and clears SSIP.
//   S-mode phase 3 (quiet): SIE and SSIE both on, source cleared.
//   Poll a bounded window and require the trap count to stay 1 and
//   sip.SSIP to read 0.
//
// All waits are bounded by instruction budgets, never open ended, so
// a broken machine yields FAIL lines, not a hang. The printed lines
// carry only deterministic values (register readbacks, small counts,
// fixed addresses); the one trap's sepc lands on a fixed linked
// address, so the output is byte-identical across runs. A 64-bit
// FNV-1a checksum over the verdict-relevant measured values is
// printed for run-to-run comparison.

#include "../uart.h"

#define SCAUSE_S_SOFT 0x8000000000000001UL  // supervisor interrupt, SSI code

#define SIE_SSIE   0x2UL  // sie bit 1: the per-interrupt enable under test
#define SSTATUS_SIE 0x2UL  // sstatus bit 1: the global gate, open the whole run

#define SSC_GATED_READS 2000000UL   // phase-1 observation window, in iterations
#define SSC_QUIET_READS 2000000UL   // phase-3 quiet window, in iterations
#define SSC_TRAP_WAIT   10000000UL  // bounded wait for the one trap

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U

static volatile unsigned long m_regs[8];  // mscratch points here
static volatile unsigned long s_regs[8];  // sscratch points here
volatile unsigned long s_done;            // raised by the S-mode handler

static volatile unsigned long gated_reads_done;
static volatile unsigned long quiet_reads_done;
static volatile unsigned long ssip_gated;   // sip.SSIP seen during phase 1

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

extern void m_trap_entry(void);
extern void s_trap_entry(void);
extern char ssc_loop[];
extern char ssc_done[];
void smode_main(void);

// Drop from M-mode to S-mode at smode_main. S-mode starts with
// sstatus.SIE = 1: sret copies sstatus.SPIE into SIE, so SPIE is set
// before the sret. sie is clear at the drop (the gate under test
// starts closed). Never returns.
static void drop_to_smode(void) {
    unsigned long v;

    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    v |= (1UL << 8);  // SPP = 1 (S-mode)
    v |= (1UL << 5);  // SPIE = 1, so sret sets SIE = 1 on entry
    __asm__ volatile("csrw sstatus, %0" :: "r"(v));
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    v = (v & ~(3UL << 11)) | (1UL << 11);  // MPP = 01, consistent view
    __asm__ volatile("csrw mstatus, %0" :: "r"(v));
    __asm__ volatile("la t0, smode_main\n"
                     "csrw sepc, t0\n"
                     "sret" ::: "t0");
    for (;;)
        __asm__ volatile("wfi");
}

// Phase 1: gate closed (SIE = 1, SSIE = 0). Run the bounded poll
// window, watching sip.SSIP. The rdcycle read is the loop sink so
// the compiler cannot fold the window away; the trap handler is
// installed, so a spurious trap would move s_regs[0] and fail the
// checks below. Marked noinline so the window is emitted exactly
// once.
static volatile unsigned long ssc_sink;
__attribute__((noinline)) static void gated_window(void) {
    unsigned long i;
    for (i = 0; i < SSC_GATED_READS; i++) {
        unsigned long c, sip;
        c = rd_cycle();
        __asm__ volatile("csrr %0, sip" : "=r"(sip));
        if (sip & SIE_SSIE)
            ssip_gated = 1;
        ssc_sink = c;
    }
    gated_reads_done = SSC_GATED_READS;
}

// Phase 2: open the gate exactly once, inside a labeled wait region.
// csrsi with immediate 2 sets sie.SSIE (bit 1). The pending SSI was
// already pending before the gate opens, so the trap lands at an
// instruction boundary inside the ssc_loop/ssc_done region; the wait
// spins on the trap counter with a decrementing budget (never a
// hang). Marked noinline so the labels are defined exactly once.
__attribute__((noinline)) static void open_gate_and_wait(void) {
    __asm__ volatile(
        ".global ssc_loop\n"
        "ssc_loop:\n"
        "csrsi sie, 2\n"      // open the gate: the pending SSI can land here
        "mv t1, %1\n"
        "1:\n"
        "ld t0, 0(%0)\n"
        "bnez t0, 2f\n"
        "addi t1, t1, -1\n"
        "bnez t1, 1b\n"
        "2:\n"
        ".global ssc_done\n"
        "ssc_done:\n"
        :
        : "r"(&s_regs[0]), "r"(SSC_TRAP_WAIT)
        : "t0", "t1", "memory");
}

// Phase 3: quiet window with both gates open and the source cleared.
// SSC_QUIET_READS rdcycle reads must produce zero traps. Marked
// noinline like the other windows.
__attribute__((noinline)) static void quiet_window(void) {
    unsigned long i;
    for (i = 0; i < SSC_QUIET_READS; i++) {
        unsigned long c = rd_cycle();
        ssc_sink = c;
    }
    quiet_reads_done = SSC_QUIET_READS;
}

static unsigned long fnv1a_64(unsigned long h, unsigned long v) {
    h ^= v;
    h *= 0x100000001b3UL;
    return h;
}

// FNV-1a 64-bit over the deterministic measured values only: the
// phase-1 trap count, the phase-1 SSIP observation, the post-open
// trap count, the trap's scause, the quiet-window extra traps, the
// final sip.SSIP bit, and the SSIE readback after the release. sepc
// is printed as a match boolean, never fed into the checksum.
static unsigned long measured_checksum(unsigned long traps_gated,
                                       unsigned long traps_quiet,
                                       unsigned long sip_final,
                                       unsigned long ssie_after) {
    unsigned long h = 0xcbf29ce484222325UL;
    h = fnv1a_64(h, traps_gated);
    h = fnv1a_64(h, ssip_gated);
    h = fnv1a_64(h, s_regs[0]);
    h = fnv1a_64(h, s_regs[2]);
    h = fnv1a_64(h, traps_quiet);
    h = fnv1a_64(h, sip_final);
    h = fnv1a_64(h, ssie_after);
    return h;
}

void smode_main(void) {
    unsigned long sstat, siev, sipv, sepc, traps_gated, traps_quiet;
    unsigned long sip_final, ssie_after, csum;

    __asm__ volatile("csrr %0, sstatus" : "=r"(sstat));
    uart_puts("S-mode entry: SIE=");
    uart_put_dec((sstat & SSTATUS_SIE) != 0);
    uart_puts("\n");
    check((sstat & SSTATUS_SIE) != 0,
          "SIE not set on S-mode entry");

    __asm__ volatile("csrr %0, sie" : "=r"(siev));
    uart_puts("S-mode entry: sie=");
    uart_put_hex(siev);
    uart_puts("\n");
    check((siev & SIE_SSIE) == 0,
          "sie.SSIE not clear at phase-1 start");

    // Phase 1: SIE = 1, SSIE = 0. The pending SSIP must stay pending
    // with no trap. The loop itself watches sip bit 1 each iteration
    // so a dropped pending bit would fail the observation check, not
    // just the trap count.
    uart_puts("phase 1: polling with SSIE clear\n");
    gated_window();
    traps_gated = s_regs[0];
    uart_puts("gated: reads-done=");
    uart_put_dec(gated_reads_done);
    uart_puts(" sip.SSIP-observed=");
    uart_put_dec(ssip_gated);
    uart_puts(" traps-during-window=");
    uart_put_dec(s_regs[0]);
    uart_puts(" (expect 2000000 / 1 / 0)\n");
    check(gated_reads_done == SSC_GATED_READS,
          "gated window did not run to completion");
    check(ssip_gated == 1,
          "sip SSIP never read pending during the gated window");
    check(s_regs[0] == 0, "trap fired with sie.SSIE clear");
    __asm__ volatile("csrr %0, sie" : "=r"(siev));
    check((siev & SIE_SSIE) == 0, "sie.SSIE changed during phase 1");

    // Phase 2: open the gate inside the labeled wait region. The only
    // 0-to-1 transition of SSIE in the whole run happens there, so the
    // one trap can only land between ssc_loop and ssc_done.
    uart_puts("phase 2: opening sie.SSIE inside the labeled wait loop\n");
    s_done = 0;
    open_gate_and_wait();

    sepc = s_regs[3];
    uart_puts("trap1: scause=");
    uart_put_hex(s_regs[2]);
    uart_puts(" sepc=");
    uart_put_hex(sepc);
    uart_puts(" expected=[");
    uart_put_hex((unsigned long)ssc_loop);
    uart_puts(",");
    uart_put_hex((unsigned long)ssc_done);
    uart_puts(") traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" sip-at-entry=");
    uart_put_hex(s_regs[4]);
    uart_puts("\n");
    check(s_regs[0] == 1, "trap never fired after SSIE set");
    __asm__ volatile("csrr %0, sie" : "=r"(siev));
    ssie_after = (siev & SIE_SSIE) != 0;
    check(ssie_after, "sie.SSIE not set after the gate-open");
    check(s_regs[2] == SCAUSE_S_SOFT,
          "scause != supervisor software interrupt");
    check(sepc >= (unsigned long)ssc_loop &&
          sepc < (unsigned long)ssc_done,
          "sepc outside the phase-2 wait loop");
    check((s_regs[4] & SIE_SSIE) != 0,
          "sip at handler entry did not show SSIP");

    // The handler cleared SSIP: the bit must read 0 now, and no
    // M-mode trap may have fired at any point.
    __asm__ volatile("csrr %0, sip" : "=r"(sipv));
    sip_final = (sipv & SIE_SSIE) != 0;
    uart_puts("after-trap: sip=");
    uart_put_hex(sipv);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" (expect 0x0 / 0)\n");
    check(sip_final == 0, "sip not cleared by the S-mode handler");
    check(m_regs[0] == 0, "M-mode trap fired during the run");

    // Phase 3: both gates open, source cleared. The count must stay
    // at 1: no re-delivery.
    uart_puts("phase 3: quiet window\n");
    quiet_window();
    traps_quiet = s_regs[0] - 1;  // traps beyond the one gate-open trap
    uart_puts("quiet: reads-done=");
    uart_put_dec(quiet_reads_done);
    uart_puts(" traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" (expect 2000000 / 1)\n");
    check(quiet_reads_done == SSC_QUIET_READS,
          "quiet window did not run to completion");
    check(s_regs[0] == 1, "re-delivery fired during the quiet window");

    csum = measured_checksum(traps_gated, traps_quiet, sip_final,
                             ssie_after);

    // Verdict section: deterministic measured values only.
    uart_puts("VERDICT gated_traps=");
    uart_put_dec(traps_gated);
    uart_puts(" gated_ssip_observed=");
    uart_put_dec(ssip_gated);
    uart_puts(" gate_open_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" quiet_extra_traps=");
    uart_put_dec(traps_quiet);
    uart_puts(" scause=");
    uart_put_hex(s_regs[2]);
    uart_puts(" sip_ssip_final=");
    uart_put_dec(sip_final);
    uart_puts(" ssie_after_open=");
    uart_put_dec(ssie_after);
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
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    // FAIL: park the hart without touching the finisher device. The
    // harness runs QEMU under timeout, so a FAIL is observable as the
    // timeout exit status (124) as well as the RESULT line.
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}

int main(void) {
    unsigned long boot_mideleg, rd, v, siev, sipv;

    uart_init();
    uart_puts("sie-ssip-clear-suppresses: S-mode SSIE gate test\n");

    // M-mode trap handler: minimal record-and-park vector, mscratch
    // at m_regs. No M-mode trap is expected after boot.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // Boot-time delegation value, for the record.
    __asm__ volatile("csrr %0, mideleg" : "=r"(boot_mideleg));
    uart_puts("boot: mideleg=");
    uart_put_hex(boot_mideleg);
    uart_puts("\n");

    // Delegate the supervisor software interrupt (mideleg bit 1) with
    // readback checks: the bit must take, and bit 9 (supervisor
    // external) must read back clear, so only the SSI is delegated.
    __asm__ volatile("csrw mideleg, %0" :: "r"(1UL << 1));
    __asm__ volatile("csrr %0, mideleg" : "=r"(rd));
    uart_puts("mideleg: write=0x2 readback=");
    uart_put_hex(rd);
    uart_puts("\n");
    check((rd & (1UL << 1)) != 0, "mideleg SSI bit did not take");
    check((rd & (1UL << 9)) == 0,
          "supervisor external interrupt delegated (bit 9 set)");

    // No exceptions delegated: keep the M-mode path clean.
    __asm__ volatile("csrw medeleg, %0" :: "r"(0UL));
    __asm__ volatile("csrr %0, medeleg" : "=r"(v));
    uart_puts("medeleg: write=0x0 readback=");
    uart_put_hex(v);
    uart_puts("\n");
    check(v == 0, "medeleg did not take 0x0 on readback");

    // S-mode trap vector and scratch, PMP opening the whole address
    // space to S-mode (lower modes default-deny), and the done-flag
    // address for the S-mode handler.
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_regs));
    s_regs[5] = (unsigned long)&s_done;
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // mcounteren: the S-mode phases use rdcycle and rdtime, so both
    // counters are granted to lower modes here.
    __asm__ volatile("csrw mcounteren, %0" :: "r"(0x7UL));  // CY|TM|IR

    // Disarm every interrupt enable before the drop: mie clear (so no
    // M-mode trap can fire while in S-mode) and mstatus.MIE clear.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrc mstatus, %0" :: "r"(1UL << 3));  // MIE off

    // Pend SSIP from M-mode: S-mode sip writes to bit 1 are dropped
    // on this hart (measured in src/sip-ssip), so the pend happens
    // here, before the drop, with a stable readback.
    __asm__ volatile("csrs mip, %0" :: "r"(1UL << 1));
    __asm__ volatile("csrr %0, mip" : "=r"(sipv));
    uart_puts("mip: after-pend=");
    uart_put_hex(sipv);
    uart_puts("\n");
    check((sipv & (1UL << 1)) != 0, "mip SSIP did not take from M-mode");

    // The gate under test starts closed: sie entirely clear.
    __asm__ volatile("csrw sie, %0" :: "r"(0UL));
    __asm__ volatile("csrr %0, sie" : "=r"(siev));
    check(siev == 0, "sie did not take 0x0 on readback");

    // Drop to S-mode with sstatus.SIE set (via SPIE); the test runs
    // in smode_main.
    drop_to_smode();
}
