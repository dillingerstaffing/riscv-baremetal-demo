// mtip_main.c: mideleg MTIP-routing experiment.
//
// Mechanism under test: mideleg bit 7 would delegate the machine
// timer interrupt (mcause code 7) to S-mode, but on QEMU 8.2.2's
// virt hart mideleg is WARL with a legalized readback of 0x1444 at
// boot, and bit 7 is not in the delegable set (see the shipped
// src/mideleg-warl module: an all-ones write reads back 0x3666,
// delegable causes 1, 2, 5, 6, 9, 10, 12, 13). The backlog item's
// premise (set bit 7, watch the timer interrupt land in S-mode)
// therefore cannot reproduce: the write is dropped by the WARL
// legalization. This module measures that directly, then answers
// the routing question anyway: with the bit-7 write attempted and
// read back clear, it arms the CLINT machine timer, drops the hart
// to S-mode, and records exactly where the armed interrupt lands.
//
// Sequence under test (M-mode boot):
//   1. Read mideleg at boot (0x1444), install the M-mode trap
//      entry (direct-mode mtvec, mscratch scratch area) and the
//      S-mode counting entry (direct-mode stvec, sscratch).
//   2. Publish the write/readback triple: write 0 -> readback,
//      write 0x80 (bit 7 only) -> readback, write 0x1444|0x80 ->
//      readback. Bit 7 must read back clear in every case: the
//      delegation was not admitted.
//   3. Set mie.MTIE and mstatus.MIE, disarm the timer
//      (mtimecmp = all-ones), open the address space to S-mode with
//      one PMP NAPOT entry, grant S-mode the cycle/time counters
//      via mcounteren, and drop to S-mode.
// S-mode:
//   4. Arm mtimecmp = mtime + 500 ticks, then spin until the M-mode
//      handler raises m_done (bounded poll, a missing trap is a
//      FAIL, not a hang).
//   5. Require exactly one M-mode trap with mcause =
//      0x8000000000000007, zero S-mode traps, mtimecmp disarmed
//      (all-ones) by the in-handler write.
//   6. A quiet rdcycle window must leave the counts unchanged:
//      exactly one trap per armed timer, no re-delivery.
//   7. A 64-bit FNV-1a checksum over the published record (boot
//      readback, the three write readbacks, the two trap counts,
//      mcause) is printed; it is identical across runs because the
//      record contains no timing-dependent fields.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.

#include "../uart.h"
#include "../preempt/clint.h"

// mideleg bit 7: would delegate the machine timer interrupt to
// S-mode if the hart admitted it. It does not.
#define MIDELEG_MTIP_BIT (1UL << 7)
// Boot readback on QEMU 8.2.2 virt: the forced H bits, also the
// legalized value of every write that sets no delegable bit.
#define MIDELEG_BOOT 0x1444UL

#define MSTATUS_MIE (1UL << 3)
#define MIE_MTIE    (1UL << 7)
#define MIP_MTIP    (1UL << 7)
// Machine timer interrupt: interrupt bit set, exception code 7.
#define MCAUSE_MTI 0x8000000000000007UL

// Timer arm distance ahead of the running mtime. QEMU's virt CLINT
// mtime runs at 10 MHz, so 500 ticks is 50 us: the pend arrives
// long after the arm write commits but the spin poll starts within
// a few instructions.
#define TIMER_AHEAD_TICKS 500UL

// Bounded windows. Plenty of time for an enabled pending interrupt
// to fire (delivery is immediate at the next instruction boundary),
// so a silent window is the mechanism, not a slow setup.
#define WAIT_BUDGET 10000000UL
#define WINDOW_CYCLES 2000000UL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final RESULT line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

static volatile unsigned long m_regs[8];  // M-mode trap scratch, mscratch points here
static volatile unsigned long s_regs[8];  // S-mode trap scratch, sscratch points here
static volatile unsigned long m_done;     // raised by the M-mode handler
static volatile unsigned long s_done;     // raised by the S-mode handler

extern void m_trap_entry(void);
extern void s_trap_entry(void);
void smode_main(void);

// Saved M-mode readbacks, published from S-mode for the record.
static unsigned long boot_saved, rd_zero_saved, rd_bit7_saved,
                     rd_forced_bit7_saved;

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

static unsigned long csr_read_mideleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mideleg" : "=r"(v));
    return v;
}

static void csr_write_mideleg(unsigned long v) {
    __asm__ volatile("csrw mideleg, %0" :: "r"(v));
}

static unsigned long read_mie(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mie" : "=r"(v));
    return v;
}

static unsigned long read_mip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mip" : "=r"(v));
    return v;
}

static unsigned long read_cycle(void) {
    unsigned long v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

// Prints one write/legalized-readback pair: the software write
// value and what mideleg reads back after the write. These pairs
// are the evidence for whether bit 7 is admitted.
static void print_pair(const char *tag, unsigned long written,
                       unsigned long readback) {
    uart_puts(tag);
    uart_puts(": write=");
    uart_put_hex(written);
    uart_puts(" readback=");
    uart_put_hex(readback);
    uart_puts("\n");
}

// Poll with a spin budget until the M-mode handler raises m_done,
// so a broken delivery is a FAIL, not a hang.
static unsigned long wait_for_mdone(unsigned long budget) {
    unsigned long spins = 0;
    while (m_done == 0 && spins < budget)
        spins++;
    return spins;
}

// Drop from M-mode to S-mode at smode_main. Mirrors the working
// construction in src/smode: sret takes the target privilege from
// sstatus.SPP. Never returns.
static void drop_to_smode(void) {
    unsigned long v;

    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    v |= (1UL << 8);  // SPP = 1 (S-mode)
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

// FNV-1a 64-bit over the published record. Deterministic across
// runs because the record holds no timing-dependent fields.
static unsigned long record_checksum(unsigned long boot,
                                     unsigned long rd_zero,
                                     unsigned long rd_bit7,
                                     unsigned long rd_forced_bit7) {
    unsigned long h = 1469598103934665603UL;
    unsigned long words[7];
    int i, j;

    words[0] = boot;
    words[1] = rd_zero;
    words[2] = rd_bit7;
    words[3] = rd_forced_bit7;
    words[4] = m_regs[0];
    words[5] = s_regs[0];
    words[6] = m_regs[2];  // mcause of the one M-mode trap
    for (i = 0; i < 7; i++)
        for (j = 0; j < 8; j++) {
            h ^= (words[i] >> (j * 8)) & 0xffUL;
            h *= 1099511628211UL;
        }
    return h;
}

void smode_main(void) {
    unsigned long spins, armed, cmp_rb, checksum;
    unsigned long quiet_start;

    uart_puts("s-mode: hart dropped to S-mode, timer disarmed\n");

    // Arm the machine timer a few hundred mtime ticks ahead. The
    // machine timer interrupt cannot be delegated (mideleg bit 7
    // read back clear above), so this pend must route to M-mode
    // even though the hart is sitting in S-mode. The armed value
    // is read back immediately, before any slow UART output: the
    // 500-tick arm distance (50 us) is shorter than printing one
    // line at 115200 baud, so the timer can legitimately fire (and
    // the handler disarm mtimecmp) during the print below.
    armed = clint_get_mtime() + TIMER_AHEAD_TICKS;
    clint_set_mtimecmp(armed);
    cmp_rb = clint_get_mtimecmp();
    check(cmp_rb == armed, "mtimecmp did not take the arm value");
    uart_puts("arm: mtimecmp=");
    uart_put_hex(cmp_rb);
    uart_puts("\n");

    spins = wait_for_mdone(WAIT_BUDGET);
    uart_puts("trap: m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" mcause=");
    uart_put_hex(m_regs[2]);
    uart_puts(" mtval=");
    uart_put_hex(m_regs[4]);
    uart_puts("\n");
    check(spins < WAIT_BUDGET, "machine timer interrupt never arrived");
    check(m_regs[0] == 1, "M-mode trap did not fire exactly once");
    check(m_regs[2] == MCAUSE_MTI, "M-mode trap mcause is not the machine timer interrupt");
    check(s_regs[0] == 0, "an S-mode trap fired: the interrupt was delegated");
    check(clint_get_mtimecmp() == ~0UL,
          "mtimecmp not disarmed by the in-handler write");

    // Quiet window: with the timer disarmed and no source re-armed,
    // the counts must not move. One armed timer means one trap.
    quiet_start = read_cycle();
    while (read_cycle() - quiet_start < WINDOW_CYCLES)
        ;
    uart_puts("quiet: m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts("\n");
    check(m_regs[0] == 1 && s_regs[0] == 0,
          "trap counts moved during the quiet window");

    checksum = record_checksum(boot_saved, rd_zero_saved, rd_bit7_saved,
                               rd_forced_bit7_saved);
    uart_puts("record checksum=");
    uart_put_hex(checksum);
    uart_puts("\n");

    if (fails == 0) {
        uart_puts("RESULT: PASS (checks=");
        uart_put_dec(checks);
        uart_puts(")\n");
        while (!(*UART0_LSR & LSR_TEMT))
            ;
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)                              // unreachable; guards against
            __asm__ volatile("wfi");          // any fall-through printing
    }
    uart_puts("RESULT: FAIL (checks=");
    uart_put_dec(checks);
    uart_puts(" fails=");
    uart_put_dec(fails);
    uart_puts(")\n");
    while (!(*UART0_LSR & LSR_TEMT))
        ;
    for (;;)
        __asm__ volatile("wfi");
}

int main(void) {
    unsigned long v, rd_zero, rd_bit7, rd_forced_bit7, mv;

    uart_init();
    uart_puts("mideleg-mtip-route: machine timer interrupt routing check\n");

    // 1. Boot state: mideleg reads the forced H bits before anything
    // runs.
    v = csr_read_mideleg();
    boot_saved = v;
    uart_puts("boot: mideleg=");
    uart_put_hex(v);
    uart_puts("\n");
    check(v == MIDELEG_BOOT, "mideleg != 0x1444 at boot");

    // Install both trap entries in direct mode; the readbacks prove
    // the vectors are really in place.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mv));
    uart_puts("trap: mtvec=");
    uart_put_hex(mv);
    uart_puts("\n");
    check(mv == (unsigned long)m_trap_entry, "mtvec did not take the handler");

    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_regs));
    s_regs[5] = (unsigned long)&s_done;
    m_regs[5] = (unsigned long)&m_done;
    __asm__ volatile("csrr %0, stvec" : "=r"(v));
    uart_puts("trap: stvec=");
    uart_put_hex(v);
    uart_puts("\n");
    check(v == (unsigned long)s_trap_entry, "stvec did not take the handler");

    // 2. The write/readback triple: bit 7 (0x80) is the machine
    // timer interrupt delegation bit. Every write drops it.
    csr_write_mideleg(0);
    rd_zero = csr_read_mideleg();
    rd_zero_saved = rd_zero;
    print_pair("mideleg", 0, rd_zero);
    check(rd_zero == MIDELEG_BOOT, "zero write did not read back 0x1444");

    csr_write_mideleg(MIDELEG_MTIP_BIT);
    rd_bit7 = csr_read_mideleg();
    rd_bit7_saved = rd_bit7;
    print_pair("mideleg", MIDELEG_MTIP_BIT, rd_bit7);
    check(rd_bit7 == MIDELEG_BOOT,
          "bit-7 write was admitted (expected 0x1444, bit 7 dropped)");
    check((rd_bit7 & MIDELEG_MTIP_BIT) == 0,
          "mideleg bit 7 reads set: delegation admitted");

    csr_write_mideleg(MIDELEG_BOOT | MIDELEG_MTIP_BIT);
    rd_forced_bit7 = csr_read_mideleg();
    rd_forced_bit7_saved = rd_forced_bit7;
    print_pair("mideleg", MIDELEG_BOOT | MIDELEG_MTIP_BIT, rd_forced_bit7);
    check(rd_forced_bit7 == MIDELEG_BOOT,
          "forced-bits|bit-7 write read back something other than 0x1444");

    // 3. No exceptions delegated: keep the M-mode path clean. The
    // machine timer interrupt is an interrupt, not an exception, so
    // medeleg does not touch it; this is belt and suspenders.
    __asm__ volatile("csrw medeleg, %0" :: "r"(0UL));
    __asm__ volatile("csrr %0, medeleg" : "=r"(mv));
    check(mv == 0, "medeleg did not take 0x0 on readback");

    // Disarm the timer before enabling anything, then confirm the
    // pending bit is clear.
    clint_set_mtimecmp(~0UL);
    check(clint_get_mtimecmp() == ~0UL, "mtimecmp not disarmed at setup");
    check((read_mip() & MIP_MTIP) == 0, "MTIP pending before arming");

    // Enable the M-mode path: mie.MTIE and mstatus.MIE. The hart is
    // about to sit in S-mode, but a non-delegated M-mode interrupt
    // is still taken in M-mode, gated by these two bits.
    __asm__ volatile("csrw mie, %0" :: "r"(MIE_MTIE));
    check(read_mie() == MIE_MTIE, "mie did not take MTIE on readback");
    __asm__ volatile("csrs mstatus, %0" :: "r"(MSTATUS_MIE));
    __asm__ volatile("csrr %0, mstatus" : "=r"(mv));
    check((mv & MSTATUS_MIE) != 0, "mstatus.MIE did not take on readback");

    // S-mode setup: PMP NAPOT entry opening the whole address space
    // (lower modes default-deny), counter access, clean pending
    // state.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");
    __asm__ volatile("csrw mcounteren, %0" :: "r"(0x7UL));  // CY|TM|IR
    __asm__ volatile("csrc mip, %0" :: "r"(MIP_MTIP));      // MTIP is read-only; no-op, kept for symmetry

    uart_puts("drop: mie=");
    uart_put_hex(read_mie());
    uart_puts(" mideleg=");
    uart_put_hex(csr_read_mideleg());
    uart_puts("\n");
    check((csr_read_mideleg() & MIDELEG_MTIP_BIT) == 0,
          "mideleg bit 7 set at drop time: delegation admitted");

    // Drop to S-mode; the arming and the verdict run in smode_main.
    drop_to_smode();

    // Never reached.
    for (;;)
        __asm__ volatile("wfi");
}
