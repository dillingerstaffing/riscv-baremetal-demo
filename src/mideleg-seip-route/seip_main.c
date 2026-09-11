// seip_main.c: mideleg bit-9 supervisor-external-interrupt routing test
// (backlog item "riscv mideleg-seip-route").
//
// Exactly one mechanism is under test: routing of the supervisor
// external interrupt (cause 9) through mideleg bit 9 to S-mode.
//
// Source note (measured, not assumed): on QEMU 8.2.2's virt hart
// mideleg is WARL with a legalized boot readback of 0x1444, and bit
// 9 (0x200, SEI) is in the delegable set, so the write takes: the
// write/readback triple publishes this. The supervisor external
// interrupt's pending bit is mip.SEIP (bit 9); an M-mode
// "csrs mip, 1<<9" sets it (measured: mip goes 0x0 -> 0x200 and a
// "csrc mip, 1<<9" clears it again), while an S-mode sip write to
// bit 9 is dropped (measured by the shipped src/sip-seip-write
// module), so the pend happens in M-mode before the drop.
//
// Sequence:
//   M-mode: install the trap handlers, record the boot mideleg,
//   write 0 to mideleg and read back the forced set, write 0x200
//   and require the readback to equal the forced set with only bit
//   9 added, write 0x1444|0x200 and require the same readback (the
//   write changed nothing else), write 0 to medeleg and read back
//   0, install stvec/sscratch, open the whole address space to
//   S-mode with one PMP NAPOT entry, grant the counters via
//   mcounteren, clear mie and mstatus.MIE (no M-mode trap can
//   fire), disarm the machine timer (mtimecmp = all-ones) so MTIP
//   is not pending, pend SEIP with SIE off (stable readback, must
//   read 0x200), then drop to S-mode.
//   S-mode: set sie.SEIE. Control: SEIP pending with SIE off,
//   poll, require 0 traps (the interrupt is masked). Main: set
//   SIE; the pending SEI is taken at the next instruction
//   boundary. Require exactly one S-mode trap with scause =
//   0x8000000000000009, sepc equal to the interrupted
//   instruction's address (captured with an in-assembly label),
//   sip showing SEIP at handler entry, and 0 M-mode traps. The
//   handler cannot clear SEIP from S-mode (sip bit 9 is read-only
//   there, per src/sip-seip-write), so it clears sstatus.SPIE as
//   well as SIE: sret restores SIE from SPIE, and without that
//   second clear the still-pending SEI would re-trap on return.
//   Quiet window: poll with SIE off, require the counts
//   unchanged: one pended SEI, one trap, no re-delivery.

#include "../uart.h"
#include "../preempt/clint.h"

#define MIDELEG_BOOT 0x1444UL       // forced bits on QEMU 8.2.2 virt
#define MIDELEG_SEI_BIT (1UL << 9)  // supervisor external interrupt
#define MIP_SEIP (1UL << 9)         // pending bit under test
#define SIE_SEIE (1UL << 9)         // S-mode enable for the SEI

#define SCAUSE_S_EXT 0x8000000000000009UL  // supervisor interrupt, SEI code

#define SSTATUS_SIE  (1UL << 1)
#define SSTATUS_SPIE (1UL << 5)
#define MSTATUS_MIE  (1UL << 3)

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final RESULT line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

static volatile unsigned long m_regs[8];  // mscratch points here
static volatile unsigned long s_regs[8];  // sscratch points here
volatile unsigned long s_done;            // raised by the S-mode handler

static unsigned long checks = 0;
static unsigned long fails = 0;

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

static unsigned long read_mip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mip" : "=r"(v));
    return v;
}

static unsigned long read_mie(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mie" : "=r"(v));
    return v;
}

static unsigned long read_cycle(void) {
    unsigned long v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

extern void m_trap_entry(void);
extern void s_trap_entry(void);
void smode_main(void);

// Saved M-mode readbacks, published from S-mode for the record.
static unsigned long rd_zero_saved, rd_9_saved, rd_forced9_saved,
                     pend_saved;

static void print_pair(const char *tag, unsigned long written,
                       unsigned long readback) {
    uart_puts(tag);
    uart_puts(": write=");
    uart_put_hex(written);
    uart_puts(" readback=");
    uart_put_hex(readback);
    uart_puts("\n");
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

// Poll with a spin budget until the done flag is raised, so a missing
// trap is a FAIL, not a hang.
static unsigned long wait_for_done(unsigned long budget) {
    unsigned long spins = 0;
    while (s_done == 0 && spins < budget)
        spins++;
    return spins;
}

static void quiet_poll(unsigned long budget) {
    unsigned long spins = 0;
    while (spins < budget)
        spins++;
}

// FNV-1a 64-bit over the published record. Deterministic across
// runs because the record holds no timing-dependent fields.
static unsigned long record_checksum(void) {
    unsigned long h = 1469598103934665603UL;
    unsigned long words[7];
    int i, j;

    words[0] = rd_zero_saved;
    words[1] = rd_9_saved;
    words[2] = rd_forced9_saved;
    words[3] = pend_saved;
    words[4] = s_regs[0];  // S-mode trap count
    words[5] = m_regs[0];  // M-mode trap count
    words[6] = s_regs[2];  // scause of the one S-mode trap
    for (i = 0; i < 7; i++)
        for (j = 0; j < 8; j++) {
            h ^= (words[i] >> (j * 8)) & 0xffUL;
            h *= 1099511628211UL;
        }
    return h;
}

void smode_main(void) {
    unsigned long spins, expected, siev, checksum;
    unsigned long quiet_start;

    // Enable the supervisor external interrupt in sie; SIE stays
    // off for the control phase.
    __asm__ volatile("csrs sie, %0" :: "r"(SIE_SEIE));
    __asm__ volatile("csrr %0, sie" : "=r"(siev));
    uart_puts("s-mode: sie=");
    uart_put_hex(siev);
    uart_puts("\n");
    check((siev & SIE_SEIE) != 0, "sie.SEIE did not take on readback");

    // Control: SEIP pending, SEIE enabled, but SIE off. The
    // interrupt is masked; no trap of either kind may arrive.
    s_done = 0;
    quiet_poll(1000000UL);
    uart_puts("control: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");
    check(s_regs[0] == 0 && m_regs[0] == 0,
          "trap arrived with SEIP pending but SIE off");

    // Main: set SIE; the pending SEI is taken at the next
    // instruction boundary, so sepc must equal the address of the
    // nop, captured here with an in-assembly local label.
    __asm__ volatile(
        "csrs sstatus, 2\n"   // sstatus.SIE = 1
        "1: nop\n"            // the pending SEI lands here
        "la %0, 1b\n"
        : "=r"(expected) :: "memory");
    spins = wait_for_done(10000000UL);
    __asm__ volatile("csrci sstatus, 2");  // SIE off before the checks
    uart_puts("sei: spins=");
    uart_put_dec(spins);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" scause=");
    uart_put_hex(s_regs[2]);
    uart_puts(" sepc=");
    uart_put_hex(s_regs[3]);
    uart_puts(" expected=");
    uart_put_hex(expected);
    uart_puts(" sip-at-entry=");
    uart_put_hex(s_regs[4]);
    uart_puts("\n");
    check(spins < 10000000UL, "supervisor external interrupt never delivered");
    check(s_regs[0] == 1, "S-mode trap did not fire exactly once");
    check(s_regs[2] == SCAUSE_S_EXT,
          "scause != supervisor external interrupt");
    check(s_regs[3] == expected,
          "sepc != interrupted-instruction address");
    check((s_regs[4] & MIP_SEIP) != 0,
          "sip at handler entry did not show SEIP");
    check(m_regs[0] == 0, "M-mode trap fired during the run");

    // Quiet window: SIE off, SEIP still pending (S-mode cannot
    // clear it; the handler masked via SPIE/SIE instead). The
    // counts must not move: one pended SEI, one trap.
    quiet_start = read_cycle();
    while (read_cycle() - quiet_start < 2000000UL)
        ;
    uart_puts("quiet: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");
    check(s_regs[0] == 1 && m_regs[0] == 0,
          "trap count moved during the quiet window");

    checksum = record_checksum();
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
    unsigned long boot_mideleg, rd_zero, rd_9, rd_forced9, pend_rb, mv;

    uart_init();
    uart_puts("mideleg-seip-route: supervisor external interrupt delegation test\n");

    // M-mode trap handler: direct-mode mtvec, mscratch at m_regs.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mv));
    uart_puts("trap: mtvec=");
    uart_put_hex(mv);
    uart_puts("\n");
    check(mv == (unsigned long)m_trap_entry, "mtvec did not take the handler");

    // Boot-time delegation value, for the record.
    boot_mideleg = csr_read_mideleg();
    uart_puts("boot: mideleg=");
    uart_put_hex(boot_mideleg);
    uart_puts("\n");
    check(boot_mideleg == MIDELEG_BOOT, "mideleg != 0x1444 at boot");

    // The write/readback triple for mideleg bit 9 (0x200, SEI).
    // Write zero first: the readback exposes the forced set. Then
    // write bit 9 alone and require the readback to equal the
    // forced set with only bit 9 added: the write must change
    // nothing else, and bit 9 must be admitted (this is the bit
    // the backlog item expects to delegate).
    csr_write_mideleg(0);
    rd_zero = csr_read_mideleg();
    rd_zero_saved = rd_zero;
    print_pair("mideleg", 0, rd_zero);
    check(rd_zero == MIDELEG_BOOT, "zero write did not read back 0x1444");

    csr_write_mideleg(MIDELEG_SEI_BIT);
    rd_9 = csr_read_mideleg();
    rd_9_saved = rd_9;
    print_pair("mideleg", MIDELEG_SEI_BIT, rd_9);
    check(rd_9 == (MIDELEG_BOOT | MIDELEG_SEI_BIT),
          "mideleg write changed more than bit 9");
    check((rd_9 & MIDELEG_SEI_BIT) != 0,
          "mideleg bit 9 not admitted: SEI not delegated");

    csr_write_mideleg(MIDELEG_BOOT | MIDELEG_SEI_BIT);
    rd_forced9 = csr_read_mideleg();
    rd_forced9_saved = rd_forced9;
    print_pair("mideleg", MIDELEG_BOOT | MIDELEG_SEI_BIT, rd_forced9);
    check(rd_forced9 == (MIDELEG_BOOT | MIDELEG_SEI_BIT),
          "forced-bits|bit-9 write read back something unexpected");

    // No exceptions delegated: keep the M-mode path clean.
    __asm__ volatile("csrw medeleg, %0" :: "r"(0UL));
    __asm__ volatile("csrr %0, medeleg" : "=r"(mv));
    uart_puts("medeleg: write=0x0 readback=");
    uart_put_hex(mv);
    uart_puts("\n");
    check(mv == 0, "medeleg did not take 0x0 on readback");

    // S-mode trap vector and scratch, PMP opening the whole address
    // space to S-mode (lower modes default-deny), the done-flag
    // address for the S-mode handler, and counter access.
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_regs));
    __asm__ volatile("csrr %0, stvec" : "=r"(mv));
    uart_puts("trap: stvec=");
    uart_put_hex(mv);
    uart_puts("\n");
    check(mv == (unsigned long)s_trap_entry, "stvec did not take the handler");
    s_regs[5] = (unsigned long)&s_done;
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");
    __asm__ volatile("csrw mcounteren, %0" :: "r"(0x7UL));  // CY|TM|IR

    // Disarm every interrupt enable before the drop: mie clear (so
    // no M-mode trap can fire while in S-mode) and mstatus.MIE
    // clear. sie.SEIE is set later in S-mode.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    check(read_mie() == 0, "mie did not take 0x0 on readback");
    __asm__ volatile("csrc mstatus, %0" :: "r"(MSTATUS_MIE));
    __asm__ volatile("csrr %0, mstatus" : "=r"(mv));
    check((mv & MSTATUS_MIE) == 0, "mstatus.MIE did not clear on readback");

    // Disarm the machine timer so MTIP (pending at boot, since
    // mtime runs past the reset mtimecmp) is not pending; the
    // pending-state readback below must show SEIP alone.
    clint_set_mtimecmp(~0UL);
    check(clint_get_mtimecmp() == ~0UL, "mtimecmp not disarmed at setup");
    check(read_mip() == 0, "mip not clean before pending SEIP");

    // Pend the supervisor external interrupt from M-mode. The
    // readback is taken with SIE off (nothing can trap in M-mode
    // anyway: mie and MIE are clear) so it is stable.
    __asm__ volatile("csrs mip, %0" :: "r"(MIP_SEIP));
    pend_rb = read_mip();
    pend_saved = pend_rb;
    uart_puts("pend: mip-after-csrs=");
    uart_put_hex(pend_rb);
    uart_puts("\n");
    check(pend_rb == MIP_SEIP, "mip readback != 0x200 after pending SEIP");

    // Drop to S-mode; the test runs in smode_main.
    drop_to_smode();

    // Never reached.
    for (;;)
        __asm__ volatile("wfi");
}
