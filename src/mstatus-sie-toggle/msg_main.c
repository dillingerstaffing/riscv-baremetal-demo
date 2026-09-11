// msg_main.c: the S-mode interrupt gate bit driven from M-mode through
// the mstatus CSR (backlog item "riscv mstatus-sie-toggle").
//
// The behavior under test: bit 1 of mstatus, the S-mode interrupt
// gate, is writable from M-mode, and M-mode's writes gate S-mode
// interrupt delivery. The privileged spec defines sstatus as a
// subset view of mstatus, so mstatus.SIE and sstatus.SIE are one
// physical bit; the C driver verifies that aliasing directly (every
// mstatus.SIE write is read back through both mstatus and sstatus),
// then observes the gating effect across two S-mode excursions.
//
// Siblings: sie-stie-gate tests a different bit (the sie.STIE
// per-interrupt enable, independent of this gate); sstatus-sie-gate
// tests this same gate bit driven from S-mode. This module drives it
// from M-mode: with the gate held closed by M-mode, a pending
// delegated supervisor software interrupt must not trap; with the
// gate opened by M-mode, the same pending interrupt must trap
// exactly once in S-mode.
//
// Machine model: QEMU 8.2.2 virt, single hart, M-mode throughout
// except the two S-mode excursions. mideleg bit 1 routes the
// supervisor software interrupt to S-mode; sie.SSIE enables it;
// mstatus.MIE and the non-delegated mie bits stay clear so the
// M-mode path is disarmed and any M-mode trap other than the two
// expected S-mode ecalls is reported and parks the hart. A PMP NAPOT
// entry opens the whole address space to S-mode (with no PMP entry,
// lower-privilege fetches fault).
//
// Phase 1 (gate closed, M-mode clears mstatus.SIE):
//   1. mideleg bit 1 set and read back; sie.SSIE set and read back.
//   2. mstatus.SIE cleared; read back clear through mstatus AND
//      through sstatus (the aliasing check).
//   3. mip.SSIP pended and read back set.
//   4. sret to the S-mode landing pad (msg_phase1 in msg_trap.S):
//      bounded rdcycle spin, then one ecall back to M-mode.
//   5. Require: S-mode traps == 0, M-mode traps == 1 (the return
//      ecall, mcause 0x9, mepc inside the pad range), mip.SSIP still
//      pending (delivery was gated, not the pending state), sie.SSIE
//      still set, the gate bit still clear.
// Phase 2 (gate open, M-mode sets mstatus.SIE):
//   6. mstatus.SIE set; read back set through mstatus AND sstatus;
//      mip.SSIP still pending.
//   7. sret to the same pad: the pending interrupt must trap exactly
//      once in S-mode with scause 0x8000000000000001 and sepc inside
//      the pad range; the handler records the cycle counter, clears
//      SSIP, and sret resumes the spin, which completes and ecalls
//      back to M-mode.
//   8. Require: S-mode traps == 1, M-mode traps == 2, mip.SSIP clear,
//      the gate bit still set, and a long post-clear quiet window
//      (cycles from the trap to the return ecall) with no further
//      traps.
//
// All waits are bounded (the spin is cycle-budgeted, the ecall path
// is the only way back), so a broken machine yields FAIL lines, not
// a hang. Expected sepc/mepc addresses come from in-asm label
// addresses taken at run time, never hardcoded. A 64-bit FNV-1a
// checksum over the deterministic measured values is printed and
// cross-checked in the PROOF.md results table.

#include "../uart.h"

typedef struct {
    unsigned long t0;      // 0
    unsigned long t1;      // 8
    unsigned long scause;  // 16
    unsigned long sepc;    // 24
    unsigned long rest[29];
} msg_save_t;

extern void msg_m_trap_entry(void);
extern void msg_s_trap_entry(void);
extern void msg_phase1(void);
extern void msg_phase2(void);
extern void smode_landing(void);
extern void smode_spin_end(void);

volatile unsigned long msg_m_traps;
volatile unsigned long msg_m_cause;
volatile unsigned long msg_m_epc;
volatile unsigned long msg_s_traps;
volatile unsigned long msg_s_cause;
volatile unsigned long msg_s_epc;
volatile unsigned long msg_s_trap_cycle;
volatile unsigned long msg_pad_lo;
volatile unsigned long msg_pad_hi;
volatile unsigned long msg_resume_pc;
volatile unsigned int msg_expect_ecall;

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

static unsigned long rd_sstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    return v;
}

static unsigned long rd_mip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mip" : "=r"(v));
    return v;
}

static unsigned long rd_sie(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, sie" : "=r"(v));
    return v;
}

static unsigned long rd_mideleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mideleg" : "=r"(v));
    return v;
}

static unsigned long rd_medeleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, medeleg" : "=r"(v));
    return v;
}

static unsigned long rd_mie(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mie" : "=r"(v));
    return v;
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

// M-mode trap handler. The two expected traps are the S-mode ecalls
// at the end of each phase: record, then redirect mepc at the resume
// label the phase driver registered and raise MPP to M-mode, so mret
// resumes the M-mode driver (mret returns to MPP, which the ecall
// trap set to S-mode; without the MPP fixup we would mret back into
// S-mode and the next M-mode CSR access in main would fault).
// Anything else is reported and parks the hart.
void msg_m_handler(void) {
    unsigned long cause, epc;
    __asm__ volatile("csrr %0, mcause" : "=r"(cause));
    __asm__ volatile("csrr %0, mepc" : "=r"(epc));
    msg_m_traps++;
    msg_m_cause = cause;
    msg_m_epc = epc;
    if (cause == 0x9UL && msg_expect_ecall != 0) {
        unsigned long ms;
        __asm__ volatile("csrr %0, mstatus" : "=r"(ms));
        ms = (ms & ~(3UL << 11)) | (3UL << 11);  // MPP = M-mode
        __asm__ volatile("csrw mstatus, %0" :: "r"(ms));
        __asm__ volatile("csrw mepc, %0" :: "r"(msg_resume_pc));
        msg_expect_ecall = 0;
        return;
    }
    uart_puts("\nUNEXPECTED M-mode trap: mcause=");
    uart_put_hex(cause);
    uart_puts(" mepc=");
    uart_put_hex(epc);
    uart_puts("\nRESULT: FAIL (unexpected M-mode trap)\n");
    for (;;)
        __asm__ volatile("wfi");
}

// S-mode trap handler: the one expected supervisor software trap.
// Records scause/sepc and the cycle counter, then clears the pending
// SSIP bit (the source is level-triggered, so leaving it set would
// re-fire).
void msg_s_handler(msg_save_t *s) {
    msg_s_cause = s->scause;
    msg_s_epc = s->sepc;
    msg_s_traps++;
    msg_s_trap_cycle = rd_cycle();
    __asm__ volatile("csrc sip, %0" :: "r"(1UL << 1));  // clear SSIP
}

static unsigned long fnv1a_64(unsigned long h, unsigned long v) {
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xffUL;
        h *= 0x100000001b3UL;
    }
    return h;
}

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U

int main(void) {
    unsigned long mideleg_boot, medeleg_boot, mie_boot, mstatus_boot;
    unsigned long sie_boot, mideleg_rb, sie_rb, mstatus_p1, sstatus_p1;
    unsigned long mip_pended, mstatus_p2, sstatus_p2, mip_p2, mip_final;
    unsigned long sepc_in_range, quiet_cycles, csum;
    int in_range;

    uart_init();
    uart_puts("mstatus-sie-toggle: S-mode interrupt gate driven from M-mode via mstatus.SIE\n");

    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)msg_m_trap_entry));
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)msg_s_trap_entry));

    // PMP: open the whole address space to S-mode (no entry means
    // lower-privilege accesses fault).
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    mideleg_boot = rd_mideleg();
    medeleg_boot = rd_medeleg();
    mie_boot = rd_mie();
    mstatus_boot = rd_mstatus();
    sie_boot = rd_sie();
    uart_puts("boot: mideleg=");
    uart_put_hex(mideleg_boot);
    uart_puts(" medeleg=");
    uart_put_hex(medeleg_boot);
    uart_puts(" mie=");
    uart_put_hex(mie_boot);
    uart_puts(" mstatus=");
    uart_put_hex(mstatus_boot);
    uart_puts(" sie=");
    uart_put_hex(sie_boot);
    uart_puts("\n");
    // S-mode ecalls must trap to M-mode: medeleg bit 9 (S-mode ecall)
    // must be clear.
    check((medeleg_boot & (1UL << 9)) == 0,
          "boot medeleg delegates S-mode ecall away from M-mode");

    // Disarm the M-mode interrupt path so the pending SSI cannot trap
    // in M-mode: mie clear, mstatus.MIE clear. sie.SSIE is set later;
    // sie is a subset view of mie, so its readback below is the same
    // physical bit.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrci mstatus, 8");  // MIE off
    check(rd_mie() == 0, "mie did not clear");

    // S-mode needs cycle-counter access for the bounded spin.
    __asm__ volatile("csrw mcounteren, %0" :: "r"(0x7UL));  // CY|TM|IR

    // Delegate the supervisor software interrupt to S-mode.
    __asm__ volatile("csrs mideleg, %0" :: "r"(1UL << 1));
    mideleg_rb = rd_mideleg();
    uart_puts("setup: mideleg-after-delegate=");
    uart_put_hex(mideleg_rb);
    uart_puts("\n");
    check((mideleg_rb & (1UL << 1)) != 0,
          "mideleg bit 1 did not stick");

    // Enable the supervisor software interrupt. sie is the S-mode
    // view of mie; setting SSIE here is what the S-mode trap logic
    // consults.
    __asm__ volatile("csrs sie, %0" :: "r"(1UL << 1));
    sie_rb = rd_sie();
    uart_puts("setup: sie-after-enable=");
    uart_put_hex(sie_rb);
    uart_puts("\n");
    check((sie_rb & (1UL << 1)) != 0, "sie.SSIE did not stick");

    // Phase 1: M-mode closes the gate through the mstatus CSR.
    __asm__ volatile("csrci mstatus, 2");  // mstatus.SIE = 0
    mstatus_p1 = rd_mstatus();
    sstatus_p1 = rd_sstatus();
    uart_puts("phase1: mstatus-after-clear=");
    uart_put_hex(mstatus_p1);
    uart_puts(" sstatus-after-clear=");
    uart_put_hex(sstatus_p1);
    uart_puts("\n");
    check((mstatus_p1 & (1UL << 1)) == 0,
          "mstatus.SIE did not clear");
    // Aliasing: the same write must be visible as sstatus.SIE clear;
    // the two CSR names address one physical bit.
    check((sstatus_p1 & (1UL << 1)) == 0,
          "sstatus.SIE did not follow the mstatus.SIE clear (aliasing)");

    // Pend the supervisor software interrupt.
    __asm__ volatile("csrs mip, %0" :: "r"(1UL << 1));
    mip_pended = rd_mip();
    uart_puts("phase1: mip-after-pend=");
    uart_put_hex(mip_pended);
    uart_puts("\n");
    check((mip_pended & (1UL << 1)) != 0,
          "mip.SSIP did not pend");

    // pad_hi is the address of the ecall instruction itself
    // (smode_spin_end labels it), so the bounds below are inclusive.
    msg_pad_lo = (unsigned long)smode_landing;
    msg_pad_hi = (unsigned long)smode_spin_end;
    uart_puts("phase1: pad=[");
    uart_put_hex(msg_pad_lo);
    uart_puts(",");
    uart_put_hex(msg_pad_hi);
    uart_puts("]\n");

    // Phase 1 excursion: S-mode spins a bounded window with the gate
    // closed, then ecalls back. No S-mode trap may fire.
    msg_phase1();

    uart_puts("phase1: s_traps=");
    uart_put_dec(msg_s_traps);
    uart_puts(" m_traps=");
    uart_put_dec(msg_m_traps);
    uart_puts(" mcause=");
    uart_put_hex(msg_m_cause);
    uart_puts(" mepc=");
    uart_put_hex(msg_m_epc);
    uart_puts("\n");
    check(msg_s_traps == 0,
          "S-mode trap fired with the gate closed (gate did not hold)");
    check(msg_m_traps == 1, "M-mode trap count != 1 after phase 1");
    check(msg_m_cause == 0x9UL,
          "phase-1 M-mode trap was not the S-mode ecall");
    check(msg_m_epc >= msg_pad_lo && msg_m_epc <= msg_pad_hi,
          "phase-1 ecall mepc outside the landing pad range");
    check((rd_mip() & (1UL << 1)) != 0,
          "mip.SSIP did not stay pending through phase 1");
    check((rd_sie() & (1UL << 1)) != 0,
          "sie.SSIE did not stay set through phase 1");
    check((rd_mstatus() & (1UL << 1)) == 0,
          "gate bit did not stay clear through phase 1");

    // Phase 2: M-mode opens the gate through the mstatus CSR. The
    // still-pending interrupt must now trap exactly once in S-mode.
    __asm__ volatile("csrsi mstatus, 2");  // mstatus.SIE = 1
    mstatus_p2 = rd_mstatus();
    sstatus_p2 = rd_sstatus();
    mip_p2 = rd_mip();
    uart_puts("phase2: mstatus-after-set=");
    uart_put_hex(mstatus_p2);
    uart_puts(" sstatus-after-set=");
    uart_put_hex(sstatus_p2);
    uart_puts(" mip=");
    uart_put_hex(mip_p2);
    uart_puts("\n");
    check((mstatus_p2 & (1UL << 1)) != 0,
          "mstatus.SIE did not set");
    check((sstatus_p2 & (1UL << 1)) != 0,
          "sstatus.SIE did not follow the mstatus.SIE set (aliasing)");
    check((mip_p2 & (1UL << 1)) != 0,
          "mip.SSIP did not stay pending into phase 2");

    msg_phase2();

    in_range = (msg_s_epc >= msg_pad_lo && msg_s_epc <= msg_pad_hi);
    sepc_in_range = in_range ? 1UL : 0UL;
    // Quiet window teeth: cycles from the S-mode trap to the return
    // ecall, i.e. how long the spin ran after the handler cleared the
    // pending bit with the gate still open.
    quiet_cycles = rd_cycle() - msg_s_trap_cycle;
    uart_puts("phase2: s_traps=");
    uart_put_dec(msg_s_traps);
    uart_puts(" scause=");
    uart_put_hex(msg_s_cause);
    uart_puts(" sepc=");
    uart_put_hex(msg_s_epc);
    uart_puts(" m_traps=");
    uart_put_dec(msg_m_traps);
    uart_puts(" quiet-cycles=");
    uart_put_dec(quiet_cycles);
    uart_puts("\n");
    check(msg_s_traps == 1,
          "S-mode trap did not fire exactly once with the gate open");
    check(msg_s_cause == 0x8000000000000001UL,
          "scause != supervisor software interrupt");
    check(in_range, "sepc outside the landing pad range");
    check(msg_m_traps == 2, "M-mode trap count != 2 after phase 2");
    check(msg_m_cause == 0x9UL,
          "phase-2 M-mode trap was not the S-mode ecall");
    mip_final = rd_mip();
    check((mip_final & (1UL << 1)) == 0,
          "mip.SSIP not cleared by the S-mode handler");
    check((rd_mstatus() & (1UL << 1)) != 0,
          "gate bit did not stay set through phase 2");
    // Quiet window: after the handler cleared SSIP the spin ran on
    // with the gate open; the trap count must not have moved past
    // the single expected trap, and the window must have been long.
    check(msg_s_traps == 1, "trap count moved during the quiet window");
    check(quiet_cycles > 100000UL, "quiet window too short to count");

    csum = 0xcbf29ce484222325UL;
    csum = fnv1a_64(csum, mideleg_rb);
    csum = fnv1a_64(csum, sie_rb);
    csum = fnv1a_64(csum, (mstatus_p1 >> 1) & 1UL);
    csum = fnv1a_64(csum, (sstatus_p1 >> 1) & 1UL);
    csum = fnv1a_64(csum, (mip_pended >> 1) & 1UL);
    csum = fnv1a_64(csum, (mstatus_p2 >> 1) & 1UL);
    csum = fnv1a_64(csum, (sstatus_p2 >> 1) & 1UL);
    csum = fnv1a_64(csum, msg_s_traps);
    csum = fnv1a_64(csum, msg_m_traps);
    csum = fnv1a_64(csum, msg_s_cause);
    csum = fnv1a_64(csum, sepc_in_range);
    csum = fnv1a_64(csum, (mip_final >> 1) & 1UL);
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
