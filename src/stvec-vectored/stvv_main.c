// stvv_main.c: S-mode vectored stvec offset test (backlog item 133).
//
// One mechanism: with stvec MODE=1 (vectored), an interrupt with code
// C traps to BASE + 4*C, so each delegated interrupt lands at a
// distinct handler address.
//
// M-mode setup: probe Sstc (menvcfg.STCE, required: the timer phase
// arms stimecmp), delegate the supervisor timer interrupt (mideleg
// bit 5) and the supervisor external interrupt (mideleg bit 9) to
// S-mode, park both timer comparators, open the address space with one
// PMP NAPOT R/W/X entry, grant S-mode counter access (mcounteren),
// program the PLIC hart-0 S-mode context (context 1) for UART0 source
// 10, then install a vectored stvec over a 16-entry table of single
// 4-byte jal stubs (stvv_trap.S) and drop to S-mode.
//
// S-mode phase 1: arm stimecmp = time + delta with sie.STIE set. The
// supervisor timer interrupt must trap exactly once with
// scause = 0x8000000000000005, landing at BASE + 20 (entry 5), and the
// handler must disarm the source (stimecmp reads back ~0).
//
// S-mode phase 2: assert the UART0 interrupt through the PLIC in
// internal loopback (same construction as src/plic and
// src/mideleg-route: no console output while loopback is on), then
// enable sie.SEIE. The supervisor external interrupt must trap exactly
// once with scause = 0x8000000000000009, landing at BASE + 36 (entry 9),
// claiming source 10 and reading the looped-back byte.
//
// PASS requires: exactly one trap per interrupt type, both scause
// values exact, both landing addresses equal to the stub entry
// addresses recorded by the stubs themselves (BASE+20 and BASE+36),
// zero unexpected traps, and no M-mode trap during setup.

#include "../uart.h"

extern void stvv_table(void);
extern void stvv_vec5(void);
extern void stvv_vec9(void);
extern void m_trap_entry(void);
void smode_main(void);

// Event log written by stvv_trap.S (no C in the trap path).
volatile unsigned long stvv_save[32];      // sscratch points here
volatile unsigned long stvv_ev_count;     // traps taken
volatile unsigned long stvv_ev_scause[4]; // scause per trap
volatile unsigned long stvv_ev_land[4];   // landing address per trap
volatile unsigned long stvv_ev_idx[4];    // table entry index per trap
volatile unsigned long stvv_unexpected;   // traps of neither armed type
volatile unsigned long stvv_timer_done;   // raised by the timer stub path
volatile unsigned long stvv_ext_done;     // raised by the external stub path
volatile unsigned long stvv_claimed;      // PLIC claim id of the external trap

// M-mode trap record (setup window); any trap here parks the hart.
volatile unsigned long m_regs[8];

#define SCAUSE_S_TIMER 0x8000000000000005UL
#define SCAUSE_S_EXT   0x8000000000000009UL

#define CSR_STIMECMP 0x14d
#define MENVCFG_STCE (1UL << 63)
#define CLINT_MTIMECMP 0x02004000UL

// QEMU virt PLIC: context 1 is hart 0's S-mode context.
#define PLIC_PRIO(s)  (0x0c000000UL + 4UL * (unsigned long)(s))
#define PLIC_PENDING  0x0c001000UL
#define PLIC_ENABLE1  0x0c002080UL
#define PLIC_THRESH1  0x0c201000UL
#define PLIC_CLAIM1   0x0c201004UL
#define UART_IRQ 10

#define UART0_BASE 0x10000000UL
#define U_THR  0x00
#define U_IER  0x01
#define U_MCR  0x04
#define U_LSR  0x05
#define IER_RDI  0x01
#define MCR_LOOP 0x10
#define LSR_THRE 0x20

#define TIMER_DELTA 2000000UL  // stimecmp ticks ahead of now (10 MHz time)

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long csr_read_menvcfg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, menvcfg" : "=r"(v));
    return v;
}

static void park(void) {
    for (;;)
        __asm__ volatile("wfi");
}

// Drop from M-mode to S-mode at smode_main. sret takes the target
// privilege from sstatus.SPP. Never returns.
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
    park();
}

// S-mode: arm the supervisor timer interrupt via stimecmp and wait for
// the single trap. Returns 0 on success, nonzero on timeout.
static int phase_timer(unsigned long base) {
    unsigned long now, cmp, spins;

    __asm__ volatile("rdtime %0" : "=r"(now));
    cmp = now + TIMER_DELTA;
    __asm__ volatile("csrs sie, %0" :: "r"(1UL << 5));  // STIE
    __asm__ volatile("csrw 0x14d, %0" :: "r"(cmp));     // stimecmp
    uart_puts("timer: armed (delta=");
    uart_put_dec(TIMER_DELTA);
    uart_puts(")\n");

    __asm__ volatile("csrsi sstatus, 2");  // SIE = 1
    spins = 0;
    while (stvv_timer_done == 0 && spins < 100000000UL) {
        __asm__ volatile("wfi");
        spins++;
    }
    __asm__ volatile("csrci sstatus, 2");  // SIE = 0
    __asm__ volatile("csrc sie, %0" :: "r"(1UL << 5));  // STIE off
    if (stvv_timer_done == 0) {
        uart_puts("  FAIL: timer interrupt never fired\n");
        return 1;
    }

    __asm__ volatile("csrr %0, 0x14d" : "=r"(cmp));  // stimecmp readback
    uart_puts("timer: ev_count=");
    uart_put_dec(stvv_ev_count);
    uart_puts(" scause=");
    uart_put_hex(stvv_ev_scause[0]);
    uart_puts(" landing=");
    uart_put_hex(stvv_ev_land[0]);
    uart_puts(" idx=");
    uart_put_dec(stvv_ev_idx[0]);
    uart_puts(" (expect ");
    uart_put_hex(base + 20);
    uart_puts(") stimecmp=");
    uart_put_hex(cmp);
    uart_puts("\n");
    check(stvv_ev_count == 1, "timer trap did not fire exactly once");
    check(stvv_ev_scause[0] == SCAUSE_S_TIMER,
          "timer scause != supervisor timer interrupt");
    check(stvv_ev_land[0] == base + 20,
          "timer trap did not land at BASE + 20");
    check(stvv_ev_idx[0] == 5, "timer trap did not use entry 5");
    check(cmp == ~0UL, "stimecmp not disarmed by the handler");
    return 0;
}

// S-mode: assert the UART0 interrupt through the PLIC in internal
// loopback, then enable SEIE and wait for the single trap. Returns 0
// on success, nonzero on timeout. Nothing is printed while loopback
// is on (bytes would be swallowed by the receiver).
static int phase_ext(unsigned long base) {
    volatile unsigned char *u_thr = (volatile unsigned char *)(UART0_BASE + U_THR);
    volatile unsigned char *u_ier = (volatile unsigned char *)(UART0_BASE + U_IER);
    volatile unsigned char *u_mcr = (volatile unsigned char *)(UART0_BASE + U_MCR);
    volatile unsigned char *u_lsr = (volatile unsigned char *)(UART0_BASE + U_LSR);
    volatile unsigned int *plic_pending = (volatile unsigned int *)PLIC_PENDING;
    unsigned char mcr_save = *u_mcr, ier_save = *u_ier;
    unsigned long spins;

    if ((*u_lsr & 0x01) != 0)
        (void)*u_thr;  // drain any stale receive byte
    *u_mcr = (unsigned char)(mcr_save | MCR_LOOP);
    *u_ier = (unsigned char)(ier_save | IER_RDI);
    while ((*u_lsr & LSR_THRE) == 0)
        ;
    *u_thr = 'Q';
    spins = 0;
    while ((((*plic_pending >> UART_IRQ) & 1U) == 0) && spins < 1000000UL)
        spins++;
    *u_mcr = mcr_save;  // loopback off: reports reach the console
    if (((*plic_pending >> UART_IRQ) & 1U) == 0) {
        uart_puts("  FAIL: PLIC pending bit never set\n");
        *u_ier = ier_save;
        return 1;
    }
    uart_puts("ext: asserted, pending observed\n");

    __asm__ volatile("csrs sie, %0" :: "r"(1UL << 9));  // SEIE
    __asm__ volatile("csrsi sstatus, 2");  // SIE = 1
    spins = 0;
    while (stvv_ext_done == 0 && spins < 100000000UL) {
        __asm__ volatile("wfi");
        spins++;
    }
    __asm__ volatile("csrci sstatus, 2");   // SIE = 0
    __asm__ volatile("csrc sie, %0" :: "r"(1UL << 9));  // SEIE off
    *u_ier = ier_save;
    if (stvv_ext_done == 0) {
        uart_puts("  FAIL: external interrupt never fired\n");
        return 1;
    }

    uart_puts("ext: ev_count=");
    uart_put_dec(stvv_ev_count);
    uart_puts(" scause=");
    uart_put_hex(stvv_ev_scause[1]);
    uart_puts(" landing=");
    uart_put_hex(stvv_ev_land[1]);
    uart_puts(" idx=");
    uart_put_dec(stvv_ev_idx[1]);
    uart_puts(" (expect ");
    uart_put_hex(base + 36);
    uart_puts(") claim=");
    uart_put_dec(stvv_claimed);
    uart_puts("\n");
    check(stvv_ev_count == 2, "external trap is not the second trap");
    check(stvv_ev_scause[1] == SCAUSE_S_EXT,
          "external scause != supervisor external interrupt");
    check(stvv_ev_land[1] == base + 36,
          "external trap did not land at BASE + 36");
    check(stvv_ev_idx[1] == 9, "external trap did not use entry 9");
    check(stvv_claimed == UART_IRQ, "PLIC claim did not return UART IRQ");
    return 0;
}

void smode_main(void) {
    unsigned long v, base, mode, v5, v9;

    __asm__ volatile("csrr %0, stvec" : "=r"(v));
    base = v & ~3UL;
    mode = v & 3UL;
    uart_puts("smode: stvec=");
    uart_put_hex(v);
    uart_puts(" base=");
    uart_put_hex(base);
    uart_puts(" mode=");
    uart_put_dec(mode);
    uart_puts("\n");
    check(mode == 1, "stvec MODE != 1 (vectored)");
    check(base == (unsigned long)stvv_table, "stvec base != stvv_table");

    // The table lays the stubs out itself: entry 5 must sit at
    // BASE + 20 and entry 9 at BASE + 36, one 4-byte jal each.
    v5 = (unsigned long)stvv_vec5;
    v9 = (unsigned long)stvv_vec9;
    uart_puts("table: base=");
    uart_put_hex(base);
    uart_puts(" vec5=");
    uart_put_hex(v5);
    uart_puts(" (expect ");
    uart_put_hex(base + 20);
    uart_puts(") vec9=");
    uart_put_hex(v9);
    uart_puts(" (expect ");
    uart_put_hex(base + 36);
    uart_puts(")\n");
    check(v5 == base + 20, "entry 5 address != BASE + 20");
    check(v9 == base + 36, "entry 9 address != BASE + 36");

    if (phase_timer(base) == 0)
        phase_ext(base);

    check(stvv_ev_count == 2, "total trap count != 2");
    check(stvv_unexpected == 0, "unexpected traps fired");

    if (fails == 0) {
        uart_puts("RESULT: PASS\n");
        __asm__ volatile("li t1, 0x100000\n"   // virt test-device finisher
                         "li t2, 0x5555\n"
                         "sw t2, 0(t1)" ::: "t1", "t2");
    } else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    park();
}

int main(void) {
    unsigned long rd, rd_zero, w, v;
    volatile unsigned int *p;

    uart_init();
    uart_puts("stvec-vectored: S-mode vectored stvec offset test\n");

    // M-mode trap handler for the setup window (direct mode, mscratch
    // at m_regs). Any M-mode trap parks the hart.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // Sstc is required: the timer phase arms stimecmp directly.
    v = csr_read_menvcfg();
    __asm__ volatile("csrs menvcfg, %0" :: "r"(MENVCFG_STCE));
    v = csr_read_menvcfg();
    uart_puts("sstc: stce=");
    uart_put_dec((v & MENVCFG_STCE) ? 1 : 0);
    uart_puts("\n");
    check((v & MENVCFG_STCE) != 0, "Sstc not present: stimecmp unusable");

    // Delegate ONLY the supervisor timer interrupt (mideleg bit 5) and
    // the supervisor external interrupt (mideleg bit 9). As in
    // src/mideleg-route, this hart ORs hypervisor interrupt bits into
    // mideleg after every write, so the zero-write readback exposes the
    // forced set and the real write must add exactly bits 5 and 9.
    __asm__ volatile("csrw mideleg, %0" :: "r"(0UL));
    __asm__ volatile("csrr %0, mideleg" : "=r"(rd_zero));
    uart_puts("mideleg: write=0x0 readback=");
    uart_put_hex(rd_zero);
    uart_puts("\n");
    w = (1UL << 5) | (1UL << 9);
    __asm__ volatile("csrw mideleg, %0" :: "r"(w));
    __asm__ volatile("csrr %0, mideleg" : "=r"(rd));
    uart_puts("mideleg: write=0x220 readback=");
    uart_put_hex(rd);
    uart_puts("\n");
    check(rd == (rd_zero | w), "mideleg write changed more than bits 5,9");
    check((rd & (1UL << 5)) != 0, "supervisor timer interrupt not delegated");
    check((rd & (1UL << 9)) != 0, "supervisor external interrupt not delegated");

    // No exceptions delegated: keep the M-mode path clean.
    __asm__ volatile("csrw medeleg, %0" :: "r"(0UL));
    __asm__ volatile("csrr %0, medeleg" : "=r"(rd));
    uart_puts("medeleg: write=0x0 readback=");
    uart_put_hex(rd);
    uart_puts("\n");
    check(rd == 0, "medeleg did not take 0x0 on readback");

    // S-mode needs the counters; park both timer comparators so no
    // stale pending interrupt fires when S-mode enables its interrupt.
    __asm__ volatile("csrw mcounteren, %0" :: "r"(0x7UL));
    *(volatile unsigned long *)CLINT_MTIMECMP = ~0UL;
    __asm__ volatile("csrw 0x14d, %0" :: "r"(~0UL));  // stimecmp = ~0

    // One PMP NAPOT R/W/X entry opens the whole address space to
    // S-mode (lower modes default-deny with no entry programmed).
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // Vectored stvec over the 16-entry stub table; sscratch at the
    // trap save area. MODE=1 is the two low bits of the written value.
    w = (unsigned long)stvv_table | 1UL;
    __asm__ volatile("csrw stvec, %0" :: "r"(w));
    __asm__ volatile("csrr %0, stvec" : "=r"(rd));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)stvv_save));
    uart_puts("stvec: written=");
    uart_put_hex(w);
    uart_puts(" readback=");
    uart_put_hex(rd);
    uart_puts("\n");
    check(rd == w, "stvec readback != written value");

    // Program the PLIC supervisor context (context 1): priority 1 for
    // the UART source, enable bit 10, threshold 0. Read every write.
    p = (volatile unsigned int *)PLIC_PRIO(UART_IRQ);
    *p = 1;
    p = (volatile unsigned int *)PLIC_ENABLE1;
    v = *p;
    *p = v | (1U << UART_IRQ);
    p = (volatile unsigned int *)PLIC_THRESH1;
    *p = 0;
    uart_puts("plic: priority[10]=");
    uart_put_dec(*(volatile unsigned int *)PLIC_PRIO(UART_IRQ));
    uart_puts(" enable10=");
    uart_put_dec(((*(volatile unsigned int *)PLIC_ENABLE1) >> UART_IRQ) & 1U);
    uart_puts(" thresh=");
    uart_put_dec(*(volatile unsigned int *)PLIC_THRESH1);
    uart_puts("\n");
    check(*(volatile unsigned int *)PLIC_PRIO(UART_IRQ) == 1,
          "priority[10] readback != 1");
    check(((*(volatile unsigned int *)PLIC_ENABLE1 >> UART_IRQ) & 1U) == 1,
          "ctx1 enable bit 10 readback != 1");
    check(*(volatile unsigned int *)PLIC_THRESH1 == 0,
          "ctx1 threshold readback != 0");

    check(m_regs[0] == 0, "unexpected M-mode trap during setup");

    drop_to_smode();  // never returns
    return 0;
}
