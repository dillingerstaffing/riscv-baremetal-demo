// midr_main.c: mideleg selective-routing test (backlog item 153).
//
// Exactly one mechanism is under test: selective interrupt routing
// through mideleg. The program sets mideleg to delegate ONLY the
// supervisor external interrupt (bit 9, readback-verified), then
// fires (a) a supervisor timer interrupt and (b) a PLIC supervisor
// external interrupt, and checks where each one lands:
//
//   (a) The supervisor timer interrupt (STI, mideleg bit 5 clear) is
//       pended by writing STIP in mip with mie.STIE and mstatus.MIE
//       set. It must trap to M-mode with mcause = 0x8000000000000005
//       (machine interrupt, supervisor timer code).
//   (b) The supervisor external interrupt (SEI, mideleg bit 9 set) is
//       asserted through the PLIC's hart-0 S-mode context (priority /
//       enable / threshold programmed with readbacks) by looping one
//       UART byte back into its own receiver. With sie.SEIE and
//       sstatus.SIE set it must trap to S-mode with scause =
//       0x8000000000000009 and sepc equal to the address of the
//       instruction that was about to execute when the trap was
//       taken (captured with an in-assembly label).
//
// The program boots in M-mode, runs phase (a) there, then drops to
// S-mode (stvec installed, one PMP NAPOT entry opening the whole
// address space) for phase (b).

#include "../uart.h"

// QEMU virt PLIC, base 0x0c000000. Context 0 is hart 0's M-mode
// context, context 1 is hart 0's S-mode context. Register layout
// follows the PLIC specification.
#define PLIC_BASE     0x0c000000UL
#define PLIC_PRIO(s)  (PLIC_BASE + 4UL * (unsigned long)(s))
#define PLIC_PENDING  (PLIC_BASE + 0x1000UL)    // word 0: sources 0..31
#define PLIC_ENABLE1  (PLIC_BASE + 0x2080UL)    // ctx 1, word 0
#define PLIC_THRESH1  (PLIC_BASE + 0x201000UL)   // ctx 1 threshold
#define PLIC_CLAIM1   (PLIC_BASE + 0x201004UL)   // ctx 1 claim/complete

#define UART_IRQ 10  // UART0 interrupt source on the virt machine

// UART0 (ns16550a) registers used to assert the interrupt.
#define UART0_BASE 0x10000000UL
#define U_THR  0x00  // transmitter holding register (write); RBR (read)
#define U_IER  0x01  // interrupt enable register
#define U_MCR  0x04  // modem control register
#define U_LSR  0x05  // line status register
#define IER_RDI  0x01  // received-data-available interrupt enable
#define MCR_LOOP 0x10  // internal loopback mode
#define LSR_DR   0x01  // receiver data ready
#define LSR_THRE 0x20  // transmitter holding register empty

#define MCAUSE_M_S_TIMER 0x8000000000000005UL  // machine interrupt, STI code
#define SCAUSE_S_EXT     0x8000000000000009UL  // supervisor interrupt, SEI code

static volatile unsigned long m_regs[8];  // mscratch points here
static volatile unsigned long s_regs[8];  // sscratch points here
volatile unsigned long s_done;            // raised by the S-mode handler
static unsigned char ier_save;            // UART IER before the assert window
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

static unsigned long csr_read_medeleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, medeleg" : "=r"(v));
    return v;
}

static unsigned long csr_read_mideleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mideleg" : "=r"(v));
    return v;
}

static void csr_write_medeleg(unsigned long v) {
    __asm__ volatile("csrw medeleg, %0" : : "r"(v));
}

static void csr_write_mideleg(unsigned long v) {
    __asm__ volatile("csrw mideleg, %0" : : "r"(v));
}

static volatile unsigned int *plic_reg(unsigned long addr) {
    return (volatile unsigned int *)addr;
}

static volatile unsigned char *uart_reg(unsigned long off) {
    return (volatile unsigned char *)(UART0_BASE + off);
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

// S-mode phase: the supervisor external interrupt is already pending
// (UART IRQ line asserted, PLIC context 1 programmed). Enable SEIE in
// sie, then enable S-mode interrupts. The pending interrupt is taken
// at the next instruction boundary, so sepc must equal the address of
// the nop; that address is captured with an in-assembly local label
// and returned for the comparison.
static unsigned long smode_arm_and_wait(void) {
    unsigned long expected;

    s_done = 0;
    __asm__ volatile("csrs sie, %0" :: "r"(1UL << 9));  // SEIE
    __asm__ volatile(
        "csrs sstatus, 2\n"   // sstatus.SIE = 1
        "1: nop\n"            // the pending SEI lands here
        "la %0, 1b\n"
        : "=r"(expected) :: "memory");
    while (s_done == 0)
        ;
    return expected;
}

void smode_main(void) {
    unsigned long expected, rd;

    expected = smode_arm_and_wait();

    // The S-mode handler read the looped-back byte and completed the
    // claim; restore the UART interrupt-enable register now that the
    // console path is back to normal.
    *uart_reg(U_IER) = ier_save;

    uart_puts("ext: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" scause=");
    uart_put_hex(s_regs[2]);
    uart_puts(" sepc=");
    uart_put_hex(s_regs[3]);
    uart_puts(" expected=");
    uart_put_hex(expected);
    uart_puts(" claim=");
    uart_put_dec(s_regs[4]);
    uart_puts("\n");
    check(s_regs[0] == 1, "S-mode trap did not fire exactly once");
    check(s_regs[2] == SCAUSE_S_EXT,
          "scause != supervisor external interrupt");
    check(s_regs[3] == expected,
          "sepc != interrupted-instruction address");
    check(s_regs[4] == UART_IRQ, "S-mode claim did not return UART IRQ");

    // Delegation must still be exactly as programmed after both phases.
    // mideleg is an M-mode-only CSR, so S-mode cannot read it directly:
    // issue an S-mode ecall (not delegated, medeleg=0) and let the
    // M-mode handler report the current value in m_regs[5].
    __asm__ volatile("ecall" ::: "memory");
    rd = m_regs[5];
    uart_puts("final: mideleg=");
    uart_put_hex(rd);
    uart_puts("\n");
    check(rd == mideleg_programmed, "mideleg changed across the run");

    if (fails == 0) {
        uart_puts("RESULT: PASS\n");
    } else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    __asm__ volatile("csrci sstatus, 2");  // SIE off before parking
    for (;;)
        __asm__ volatile("wfi");
}

int main(void) {
    unsigned long boot_medeleg, boot_mideleg, rd, rd_zero, v;
    unsigned char mcr_save;
    unsigned long spins;

    uart_init();
    uart_puts("mideleg-route: selective delegation routing test\n");

    // M-mode trap handler: direct-mode mtvec, mscratch at m_regs.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // Boot-time delegation values, for the record.
    boot_medeleg = csr_read_medeleg();
    boot_mideleg = csr_read_mideleg();
    uart_puts("boot: medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts(" mideleg=");
    uart_put_hex(boot_mideleg);
    uart_puts("\n");

    // Selective delegation: ONLY the supervisor external interrupt
    // (mideleg bit 9) among the software-writable bits. First write
    // zero and read back: this hart implements the hypervisor
    // extension, and QEMU ORs the hypervisor interrupt bits into
    // mideleg after every write (target/riscv/csr.c, rmw_mideleg64),
    // so the zero-write readback exposes exactly that forced set.
    // Then write bit 9 and require the readback to equal the forced
    // set with only bit 9 added: the write must change nothing else.
    // Bit 5 (supervisor timer) must read back clear, so the timer
    // interrupt keeps trapping to M-mode.
    csr_write_mideleg(0);
    rd_zero = csr_read_mideleg();
    uart_puts("mideleg: write=0x0 readback=");
    uart_put_hex(rd_zero);
    uart_puts("\n");
    csr_write_mideleg(1UL << 9);
    rd = csr_read_mideleg();
    uart_puts("mideleg: write=0x200 readback=");
    uart_put_hex(rd);
    uart_puts("\n");
    check(rd == (rd_zero | (1UL << 9)),
          "mideleg write changed more than bit 9");
    check((rd & (1UL << 5)) == 0,
          "supervisor timer interrupt delegated (bit 5 set)");
    mideleg_programmed = rd;

    // No exceptions delegated: keep the M-mode path clean.
    csr_write_medeleg(0);
    rd = csr_read_medeleg();
    uart_puts("medeleg: write=0x0 readback=");
    uart_put_hex(rd);
    uart_puts("\n");
    check(rd == 0, "medeleg did not take 0x0 on readback");

    // Phase (a): supervisor timer interrupt, NOT delegated, so it must
    // trap to M-mode. Pend STIP with STIE and MIE set.
    __asm__ volatile("csrs mie, %0" :: "r"(1UL << 5));      // STIE
    __asm__ volatile("csrs mstatus, %0" :: "r"(1UL << 3));  // MIE
    __asm__ volatile("csrs mip, %0" :: "r"(1UL << 5));      // pend STIP
    // The trap lands in m_trap_entry, which records mcause and clears
    // STIP so the interrupt fires exactly once.
    uart_puts("timer: m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" mcause=");
    uart_put_hex(m_regs[2]);
    uart_puts("\n");
    check(m_regs[0] == 1, "timer interrupt did not trap exactly once");
    check(m_regs[2] == MCAUSE_M_S_TIMER,
          "timer trap mcause != machine supervisor-timer");

    // Disarm M-mode interrupt state before the drop to S-mode.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrw mip, %0" :: "r"(0UL));

    // Phase (b) setup: S-mode trap vector and scratch, PMP opening the
    // whole address space to S-mode (lower modes default-deny), and
    // the done-flag address for the S-mode handler.
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_regs));
    s_regs[5] = (unsigned long)&s_done;
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // Program the PLIC supervisor context (context 1, hart 0 S-mode):
    // priority 1 for the UART source, enable bit 10, threshold 0.
    // Every write is read back.
    *plic_reg(PLIC_PRIO(UART_IRQ)) = 1;
    v = *plic_reg(PLIC_ENABLE1);
    *plic_reg(PLIC_ENABLE1) = v | (1U << UART_IRQ);
    *plic_reg(PLIC_THRESH1) = 0;
    uart_puts("plic: priority[10]=");
    uart_put_dec(*plic_reg(PLIC_PRIO(UART_IRQ)));
    uart_puts(" enable10=");
    uart_put_dec((*plic_reg(PLIC_ENABLE1) >> UART_IRQ) & 1U);
    uart_puts(" thresh=");
    uart_put_dec(*plic_reg(PLIC_THRESH1));
    uart_puts("\n");
    check(*plic_reg(PLIC_PRIO(UART_IRQ)) == 1, "priority[10] readback != 1");
    check(((*plic_reg(PLIC_ENABLE1) >> UART_IRQ) & 1U) == 1,
          "ctx1 enable bit 10 readback != 1");
    check(*plic_reg(PLIC_THRESH1) == 0, "ctx1 threshold readback != 0");

    // Assert the interrupt: internal loopback plus the receive-data
    // interrupt, then one transmitted byte. The byte lands in the
    // receiver, the UART asserts its IRQ line, the PLIC sets pending
    // bit 10. Same construction as src/plic: nothing is printed while
    // loopback is enabled (bytes would be swallowed), MCR is restored
    // as soon as the pending bit is observed, and IER_RDI stays on so
    // the line stays asserted through the claim in the S-mode handler.
    if ((*uart_reg(U_LSR) & LSR_DR) != 0)
        (void)*uart_reg(U_THR);  // drain any stale receive byte
    mcr_save = *uart_reg(U_MCR);
    ier_save = *uart_reg(U_IER);
    *uart_reg(U_MCR) = (unsigned char)(mcr_save | MCR_LOOP);
    *uart_reg(U_IER) = (unsigned char)(ier_save | IER_RDI);
    while ((*uart_reg(U_LSR) & LSR_THRE) == 0)
        ;  // wait for an empty transmitter
    *uart_reg(U_THR) = 'Q';
    spins = 0;
    while ((((*plic_reg(PLIC_PENDING) >> UART_IRQ) & 1U) == 0) &&
           spins < 1000000UL)
        spins++;
    *uart_reg(U_MCR) = mcr_save;  // loopback off: reports reach the console
    uart_puts("assert: pending10=");
    uart_put_dec((*plic_reg(PLIC_PENDING) >> UART_IRQ) & 1U);
    uart_puts(" (spins=");
    uart_put_dec(spins);
    uart_puts(")\n");
    check(spins < 1000000UL, "UART IRQ never showed pending");
    check(((*plic_reg(PLIC_PENDING) >> UART_IRQ) & 1U) == 1,
          "pending bit 10 not set after TX byte");

    // Drop to S-mode; phase (b) runs in smode_main.
    drop_to_smode();
}
