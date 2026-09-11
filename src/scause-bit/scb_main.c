// scb_main.c: scause INTERRUPT-bit probe (backlog item 125).
//
// One mechanism: the top bit of scause (bit 63, the INTERRUPT bit in
// the RISC-V privileged specification) distinguishes interrupts from
// exceptions. The program delegates a supervisor timer interrupt
// (mideleg bit 5) and the load page fault exception (medeleg bit 13)
// to S-mode, then in S-mode triggers both and records scause:
//
//   fault phase: a load from a deliberately unmapped virtual address
//       must trap with scause = 13 (bit 63 clear, load page fault),
//       stval = the faulting address.
//   timer phase: stimecmp armed a short interval ahead must fire with
//       scause = 0x8000000000000005 (bit 63 set, cause 5, supervisor
//       timer interrupt).
//
// The fault phase runs with sstatus.SIE clear (exceptions trap
// regardless of SIE), then SIE is enabled for the timer phase, so the
// two events cannot be confused. Both scause values are printed and
// the run prints RESULT: PASS only if every check holds.
//
// M-mode setup: one PMP NAPOT entry opening the whole address space
// R/W/X (lower modes default-deny with PMP implemented), mcounteren
// granting S-mode rdtime, an Sv39 table identity-mapping
// [0x80000000, 0xC0000000) with a 1 GiB megapage plus a 4 KiB UART
// page at 0x10000000, and every other entry invalid so VA 0x40000000
// is unmapped. Sstc is required (menvcfg.STCE probe, same as
// src/stvec-vectored): without it S-mode cannot arm stimecmp.

#include "../uart.h"

extern void m_trap_entry(void);
extern void s_trap_entry(void);
void smode_main(void);

// M-mode trap record: no trap should reach M-mode during the run.
volatile unsigned long m_regs[8];

// S-mode trap record, laid out for scb_trap.S:
// 0 trap count, 1 parked t1, 2 last scause, 3 last sepc, 4 last stval,
// 5 &s_timer_done, 6 parked t2, 7 fault scause (sticky),
// 8 timer scause (sticky), 9 unexpected count, 10 fault stval (sticky).
volatile unsigned long s_regs[12];
volatile unsigned long s_timer_done;
volatile unsigned long s_sstc;  // Sstc probe result, set by M-mode setup

// Sv39 table pages; BSS clearing zeroes every other entry (invalid),
// which is what the unmapped-address fault relies on.
static unsigned long root_pt[512] __attribute__((aligned(4096)));
static unsigned long l1_uart[512] __attribute__((aligned(4096)));
static unsigned long l0_uart[512] __attribute__((aligned(4096)));

// The deliberately unmapped virtual address: VPN[2]=1, and root_pt[1]
// is invalid, so the walk dies at the root lookup.
#define FAULT_VA 0x40000000UL

#define IDENT_BASE 0x80000000UL
#define IDENT_END  0xC0000000UL
#define UART_BASE  0x10000000UL

// PTE flag bits (RISC-V privileged spec, Sv39 PTE format).
#define PTE_V 0x001UL
#define PTE_R 0x002UL
#define PTE_W 0x004UL
#define PTE_X 0x008UL
#define PTE_A 0x040UL
#define PTE_D 0x080UL

#define SATP_MODE_SV39 (8UL << 60)
#define MENVCFG_STCE (1UL << 63)
#define CSR_STIMECMP 0x14d
#define TIMER_DELTA 10000UL  // 1 ms at the 10 MHz QEMU timebase

// Expected scause values (privileged spec, scause encoding:
// bit 63 = INTERRUPT, low bits = exception/interrupt code).
#define SCAUSE_LOAD_PAGE_FAULT 13UL
#define SCAUSE_S_TIMER_IRQ 0x8000000000000005UL

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

static unsigned long csr_read_menvcfg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, menvcfg" : "=r"(v));
    return v;
}

// Drop from M-mode to S-mode at smode_main. Mirrors the working
// construction in src/mideleg-route: sret takes the target privilege
// from sstatus.SPP. Never returns.
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

static unsigned long read_time(void) {
    unsigned long v;
    __asm__ volatile("rdtime %0" : "=r"(v));
    return v;
}

static void arm_stimecmp(unsigned long deadline) {
    __asm__ volatile("csrw 0x14d, %0" :: "r"(deadline));
}

static unsigned long read_stimecmp(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, 0x14d" : "=r"(v));
    return v;
}

// Fault phase: issue a load from the unmapped VA with SIE clear.
// The trap handler records scause/stval and resumes at label 1;
// after_ld is the address right after the 4-byte faulting ld
// (ld with rd=x6 is never compressed), so the recorded sepc must
// equal after_ld - 4.
static unsigned long fault_phase(void) {
    unsigned long after_ld;

    __asm__ volatile(
        "mv a0, %1\n"
        "ld t1, 0(a0)\n"   // faulting load: load page fault expected
        "1:\n"
        "la %0, 1b\n"
        : "=r"(after_ld)
        : "r"(FAULT_VA)
        : "a0", "t1", "memory");
    return after_ld;
}

void smode_main(void) {
    unsigned long after_ld, rb;

    uart_puts("s-mode: entered, translation active\n");

    // Sstc presence was probed in M-mode (menvcfg is M-mode-only, so
    // S-mode cannot re-probe); the result is carried in s_sstc.
    check(s_sstc != 0, "Sstc not present: stimecmp unusable");

    // Timer phase setup: arm stimecmp a short interval ahead and
    // enable sie.STIE. SIE stays clear through the fault phase so the
    // timer cannot fire before the fault is recorded.
    arm_stimecmp(read_time() + TIMER_DELTA);
    __asm__ volatile("csrs sie, %0" :: "r"(1UL << 5));  // STIE

    // Fault phase: load from the unmapped address.
    after_ld = fault_phase();

    uart_puts("fault: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" scause=");
    uart_put_hex(s_regs[7]);
    uart_puts(" stval=");
    uart_put_hex(s_regs[10]);
    uart_puts(" sepc=");
    uart_put_hex(s_regs[3]);
    uart_puts(" after_ld-4=");
    uart_put_hex(after_ld - 4);
    uart_puts("\n");
    check(s_regs[0] == 1, "fault phase: trap count != 1");
    check(s_regs[7] == SCAUSE_LOAD_PAGE_FAULT,
          "fault phase: scause != 13 (load page fault, bit 63 clear)");
    check(s_regs[10] == FAULT_VA, "fault phase: stval != faulting VA");
    check(s_regs[3] == after_ld - 4,
          "fault phase: sepc != address of faulting ld");

    // Timer phase: enable S-mode interrupts and wait for the armed
    // stimecmp to fire. The handler disarms stimecmp and raises the
    // done flag.
    __asm__ volatile("csrs sstatus, 2");  // SIE = 1
    while (s_timer_done == 0)
        __asm__ volatile("wfi");

    rb = read_stimecmp();
    uart_puts("timer: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" scause=");
    uart_put_hex(s_regs[8]);
    uart_puts(" stimecmp=");
    uart_put_hex(rb);
    uart_puts(" unexpected=");
    uart_put_dec(s_regs[9]);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");
    check(s_regs[0] == 2, "timer phase: total trap count != 2");
    check(s_regs[8] == SCAUSE_S_TIMER_IRQ,
          "timer phase: scause != 0x8000000000000005");
    check(s_regs[9] == 0, "unexpected S-mode traps seen");
    check(m_regs[0] == 0, "traps reached M-mode");
    check(rb == ~0UL, "stimecmp did not read back all-ones after disarm");

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
    unsigned long root_ppn, l1u_ppn, l0u_ppn;
    unsigned long ident_ppn, uart_ppn;
    unsigned long rd, rd_zero, satp, v;

    uart_init();
    uart_puts("scause-bit: scause INTERRUPT-bit probe (backlog item 125)\n");

    // M-mode trap vector: any trap here parks the hart.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // Sstc probe: try to set menvcfg.STCE and see if the bit sticks.
    // Required: S-mode arms stimecmp directly.
    __asm__ volatile("csrs menvcfg, %0" :: "r"(MENVCFG_STCE));
    v = csr_read_menvcfg();
    uart_puts("sstc: stce=");
    uart_put_dec((v & MENVCFG_STCE) ? 1 : 0);
    uart_puts("\n");
    check((v & MENVCFG_STCE) != 0, "Sstc not present: stimecmp unusable");
    s_sstc = (v & MENVCFG_STCE) ? 1 : 0;

    // PMP: with no PMP entry programmed, S-mode has no access to any
    // address (M-mode keeps full access, lower modes default-deny).
    // One NAPOT entry opens the whole address space R/W/X.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // mcounteren: rdtime is unreadable in S-mode unless M-mode grants
    // access; at reset all bits are clear.
    __asm__ volatile("csrw mcounteren, %0" :: "r"(0x7UL));  // CY|TM|IR

    // Build the Sv39 tables by hand.
    // root[2]: 1 GiB megapage leaf, identity [0x80000000, 0xC0000000)
    //   (code, data, stack, the tables themselves).
    // root[0]: pointer to the level-1 table holding the UART mapping;
    //   l1_uart[128]: pointer to a level-0 table; l0_uart[0]: leaf
    //   mapping the UART MMIO page (R/W, no X needed for MMIO).
    // Every other entry in all three tables is zero (invalid), so VA
    // 0x40000000 (VPN[2]=1) has no mapping: the walk dies at the root.
    check(((unsigned long)root_pt & 0xFFFUL) == 0, "root_pt misaligned");
    check(((unsigned long)l1_uart & 0xFFFUL) == 0, "l1_uart misaligned");
    check(((unsigned long)l0_uart & 0xFFFUL) == 0, "l0_uart misaligned");
    root_ppn = (unsigned long)root_pt >> 12;
    l1u_ppn = (unsigned long)l1_uart >> 12;
    l0u_ppn = (unsigned long)l0_uart >> 12;
    ident_ppn = IDENT_BASE >> 12;
    uart_ppn = UART_BASE >> 12;
    root_pt[2] = (ident_ppn << 10) | (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);
    root_pt[0] = (l1u_ppn << 10) | PTE_V;
    l1_uart[128] = (l0u_ppn << 10) | PTE_V;
    l0_uart[0] = (uart_ppn << 10) | (PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);

    // Delegation: load page fault (medeleg bit 13) and supervisor
    // timer interrupt (mideleg bit 5) route to S-mode. Nothing else
    // is delegated, so any other trap lands in the M-mode vector and
    // parks the hart.
    __asm__ volatile("csrw medeleg, %0" :: "r"(1UL << 13));
    rd = csr_read_medeleg();
    uart_puts("medeleg: write=0x2000 readback=");
    uart_put_hex(rd);
    uart_puts("\n");
    check(rd == (1UL << 13), "medeleg did not take 0x2000 on readback");

    // QEMU ORs the hypervisor interrupt bits into mideleg after every
    // write (target/riscv/csr.c, rmw_mideleg64; observed 0x1444 in
    // src/mideleg-route), so read back the zero-write forced set
    // first and require the bit-5 write to add exactly that bit.
    __asm__ volatile("csrw mideleg, %0" :: "r"(0UL));
    rd_zero = csr_read_mideleg();
    __asm__ volatile("csrw mideleg, %0" :: "r"(1UL << 5));
    rd = csr_read_mideleg();
    uart_puts("mideleg: write=0x0 readback=");
    uart_put_hex(rd_zero);
    uart_puts(" write=0x20 readback=");
    uart_put_hex(rd);
    uart_puts("\n");
    check(rd == (rd_zero | (1UL << 5)),
          "mideleg write changed more than bit 5");

    // S-mode trap state: direct-mode stvec, sscratch at s_regs, and
    // the done-flag address for the timer path in slot 5.
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_regs));
    s_regs[5] = (unsigned long)&s_timer_done;
    s_timer_done = 0;

    // Enable Sv39 and fence.
    __asm__ volatile("csrw satp, %0" :: "r"(SATP_MODE_SV39 | root_ppn));
    __asm__ volatile("sfence.vma" ::: "memory");
    __asm__ volatile("csrr %0, satp" : "=r"(satp));
    uart_puts("m-mode: satp=");
    uart_put_hex(satp);
    uart_puts(" (MODE=8 ASID=0)\n");
    check((satp >> 60) == 8, "satp MODE != 8 (Sv39)");
    check((satp & 0xFFFFFFFFFFFUL) == root_ppn, "satp PPN != root page");

    uart_puts("m-mode: entering S-mode\n");
    drop_to_smode();
    __builtin_unreachable();
}
