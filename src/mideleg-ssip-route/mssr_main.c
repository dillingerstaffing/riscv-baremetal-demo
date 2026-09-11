// mssr_main.c: mideleg bit-1 supervisor-software-interrupt routing
// test (backlog item "riscv mideleg-ssip-route").
//
// Exactly one mechanism is under test: routing of the supervisor
// software interrupt (cause 1) through mideleg bit 1 to S-mode.
//
// Source note (measured, not assumed): the backlog item as written
// names the CLINT msip as the interrupt source, but a pre-build
// experiment on QEMU 8.2.2 measured that a CLINT msip set drives
// mip.MSIP (bit 3, the machine software interrupt, cause 3), which
// mideleg bit 1 does not delegate: with mie.MSIE and mstatus.MIE
// armed it traps to M-mode with mcause = 0x8000000000000003 and
// zero S-mode traps. The supervisor software interrupt's own source
// is mip.SSIP (bit 1); that is what mideleg bit 1 covers, so the
// module pends SSIP. A negative control inside the run re-verifies
// the non-routing: a pending CLINT msip with SIE and SSIE on
// produces no trap of either kind.
//
// Sequence:
//   M-mode: install the trap handlers, record the boot mideleg,
//   write 0 to mideleg and read back the forced set, write bit 1 and
//   require the readback to equal the forced set with only bit 1
//   added (bit 9 must read back clear, so only the SSI is
//   delegated), write 0 to medeleg and read back 0, install
//   stvec/sscratch, open the whole address space to S-mode with one
//   PMP NAPOT entry, clear mie and mstatus.MIE, clear msip and SSIP,
//   then drop to S-mode.
//   S-mode: set sie.SSIE. Control: SIE on with nothing pending, poll,
//   require 0 traps. Negative control: set CLINT msip (32-bit
//   access; this QEMU's CLINT faults on 64-bit msip accesses), read
//   back 1, poll with SIE on while it stays pending, require 0 traps
//   of either kind, clear it, read back 0. Main: pend SSIP with SIE
//   off (stable readback, must read 0x2), set SIE; the trap lands at
//   the next instruction boundary. Require exactly one S-mode trap
//   with scause = 0x8000000000000001, sepc equal to the interrupted
//   instruction's address (captured with an in-assembly label), sip
//   showing SSIP at handler entry and 0 after the handler cleared
//   it, and 0 M-mode traps. Quiet window: poll with SIE on, require
//   the counts unchanged.

#include "../uart.h"

// QEMU virt CLINT; msip for hart 0 is a 32-bit register at the base.
#define CLINT_MSIP0 0x02000000UL

#define SCAUSE_S_SOFT 0x8000000000000001UL  // supervisor interrupt, SSI code

static volatile unsigned long m_regs[8];  // mscratch points here
static volatile unsigned long s_regs[8];  // sscratch points here
volatile unsigned long s_done;            // raised by the S-mode handler

static volatile unsigned int *const msip0 =
    (volatile unsigned int *)CLINT_MSIP0;

static unsigned long mideleg_programmed;  // mideleg readback after setup

static int fails = 0;

static void check(int cond, const char *msg) {
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

extern void m_trap_entry(void);
extern void s_trap_entry(void);
void smode_main(void);

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

void smode_main(void) {
    unsigned long spins, expected, sipv;

    __asm__ volatile("csrs sie, %0" :: "r"(1UL << 1));  // SSIE

    // Control: SIE on with nothing pending; no trap may arrive.
    s_done = 0;
    __asm__ volatile("csrs sstatus, 2");
    quiet_poll(1000000UL);
    __asm__ volatile("csrci sstatus, 2");
    uart_puts("control: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");
    check(s_regs[0] == 0 && m_regs[0] == 0,
          "spurious trap with nothing pending");

    // Negative control: a pending CLINT msip must NOT route through
    // mideleg bit 1. Set it with SIE off (stable readback), then poll
    // with SIE on while it stays pending: no trap of either kind may
    // arrive. mie.MSIE and mstatus.MIE are clear, so the M-mode path
    // is disarmed too; the readback proves the bit really stayed set.
    __asm__ volatile("csrci sstatus, 2");
    *msip0 = 1;
    sipv = *msip0;
    uart_puts("negctl: msip-readback=");
    uart_put_dec(sipv);
    uart_puts("\n");
    check(sipv == 1, "msip readback != 1 after set");
    __asm__ volatile("csrs sstatus, 2");
    quiet_poll(1000000UL);
    __asm__ volatile("csrci sstatus, 2");
    uart_puts("negctl: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" msip-still-set=");
    uart_put_dec(*msip0);
    uart_puts("\n");
    check(s_regs[0] == 0 && m_regs[0] == 0,
          "pending msip routed a trap with only mideleg bit 1 set");
    check(*msip0 == 1, "msip did not stay set through the poll");
    *msip0 = 0;
    uart_puts("negctl: msip-after-clear=");
    uart_put_dec(*msip0);
    uart_puts("\n");
    check(*msip0 == 0, "msip did not clear");

    // Main: pend the supervisor software interrupt's own source,
    // mip.SSIP, with SIE off so the readback is stable.
    __asm__ volatile("csrci sstatus, 2");
    __asm__ volatile("csrs sip, %0" :: "r"(1UL << 1));  // pend SSIP
    __asm__ volatile("csrr %0, sip" : "=r"(sipv));
    uart_puts("ssi: sip-after-pend=");
    uart_put_hex(sipv);
    uart_puts("\n");
    check(sipv == 0x2, "sip readback != 0x2 after pending SSIP");

    // Set SIE; the pending SSI is taken at the next instruction
    // boundary, so sepc must equal the address of the nop, captured
    // here with an in-assembly local label.
    s_done = 0;
    __asm__ volatile(
        "csrs sstatus, 2\n"   // sstatus.SIE = 1
        "1: nop\n"            // the pending SSI lands here
        "la %0, 1b\n"
        : "=r"(expected) :: "memory");
    spins = wait_for_done(10000000UL);
    __asm__ volatile("csrci sstatus, 2");  // SIE off before the checks
    uart_puts("ssi: spins=");
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
    check(spins < 10000000UL, "supervisor software interrupt never delivered");
    check(s_regs[0] == 1, "S-mode trap did not fire exactly once");
    check(s_regs[2] == SCAUSE_S_SOFT,
          "scause != supervisor software interrupt");
    check(s_regs[3] == expected,
          "sepc != interrupted-instruction address");
    check((s_regs[4] & (1UL << 1)) != 0,
          "sip at handler entry did not show SSIP");

    // The handler cleared SSIP: sip must read 0 now.
    __asm__ volatile("csrr %0, sip" : "=r"(sipv));
    uart_puts("ssi: sip-after-handler=");
    uart_put_hex(sipv);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");
    check(sipv == 0, "sip not cleared by the S-mode handler");
    check(m_regs[0] == 0, "M-mode trap fired during the run");

    // Quiet window: SIE on, nothing pending; the counts must not move.
    __asm__ volatile("csrs sstatus, 2");
    quiet_poll(2000000UL);
    __asm__ volatile("csrci sstatus, 2");
    uart_puts("quiet: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");
    check(s_regs[0] == 1 && m_regs[0] == 0,
          "trap count moved during the quiet window");

    // Delegation must still be exactly as programmed. mideleg is an
    // M-mode-only CSR; the programmed readback was captured in
    // M-mode before the drop, and no M-mode trap ran since, so the
    // value cannot have changed under us. Report it for the record.
    uart_puts("final: mideleg-programmed=");
    uart_put_hex(mideleg_programmed);
    uart_puts("\n");

    if (fails == 0) {
        uart_puts("RESULT: PASS\n");
    } else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}

int main(void) {
    unsigned long boot_mideleg, rd_zero, rd;

    uart_init();
    uart_puts("mideleg-ssip-route: supervisor software interrupt delegation test\n");

    // M-mode trap handler: direct-mode mtvec, mscratch at m_regs.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // Boot-time delegation value, for the record.
    boot_mideleg = csr_read_mideleg();
    uart_puts("boot: mideleg=");
    uart_put_hex(boot_mideleg);
    uart_puts("\n");

    // Delegate ONLY the supervisor software interrupt (mideleg bit 1)
    // among the software-writable bits. First write zero and read
    // back: this hart implements the hypervisor extension, and QEMU
    // ORs the hypervisor interrupt bits into mideleg after every
    // write, so the zero-write readback exposes exactly that forced
    // set. Then write bit 1 and require the readback to equal the
    // forced set with only bit 1 added: the write must change
    // nothing else. Bit 9 (supervisor external) must read back
    // clear, so the delegation is selective to the SSI.
    csr_write_mideleg(0);
    rd_zero = csr_read_mideleg();
    uart_puts("mideleg: write=0x0 readback=");
    uart_put_hex(rd_zero);
    uart_puts("\n");
    csr_write_mideleg(1UL << 1);
    rd = csr_read_mideleg();
    uart_puts("mideleg: write=0x2 readback=");
    uart_put_hex(rd);
    uart_puts("\n");
    check(rd == (rd_zero | (1UL << 1)),
          "mideleg write changed more than bit 1");
    check((rd & (1UL << 9)) == 0,
          "supervisor external interrupt delegated (bit 9 set)");
    mideleg_programmed = rd;

    // No exceptions delegated: keep the M-mode path clean.
    __asm__ volatile("csrw medeleg, %0" :: "r"(0UL));
    {
        unsigned long mv;
        __asm__ volatile("csrr %0, medeleg" : "=r"(mv));
        uart_puts("medeleg: write=0x0 readback=");
        uart_put_hex(mv);
        uart_puts("\n");
        check(mv == 0, "medeleg did not take 0x0 on readback");
    }

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

    // Disarm every interrupt enable before the drop: mie clear (so no
    // M-mode trap can fire while in S-mode) and mstatus.MIE clear.
    // sie.SSIE is set later in S-mode.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrc mstatus, %0" :: "r"(1UL << 3));  // MIE off

    // Start from a clean pending state.
    *msip0 = 0;
    __asm__ volatile("csrc mip, %0" :: "r"(1UL << 1));  // SSIP clear

    // Drop to S-mode; the test runs in smode_main.
    drop_to_smode();
}
