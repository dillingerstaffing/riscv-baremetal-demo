// stvm_main.c: stvec MODE=1 (vectored) split between synchronous
// exceptions and interrupts (backlog item "riscv stvec-vectored-mode").
//
// Exactly one mechanism is under test: where the hart goes in
// vectored mode for an exception versus an interrupt. The privileged
// spec (Trap Vector Base Address Register) states that in vectored
// mode synchronous exceptions always enter at BASE and only
// interrupts enter at BASE + 4*cause. This module measures both
// paths against that rule.
//
// Correction of the backlog premise: the backlog item as written
// claimed an illegal instruction would land at BASE+8 and an ecall at
// BASE+4*cause, i.e. that exceptions vector like interrupts. That is
// not what the spec says: only interrupts vector by cause.
// Implementing the written premise would have shipped a false claim.
// The module instead proves the true rule: exceptions enter at
// BASE, the supervisor software interrupt enters at BASE+4.
//
// Setup (M-mode): direct-mode mtvec + mscratch, delegate only the
// illegal-instruction exception (medeleg bit 2, readback-verified)
// and only the supervisor software interrupt (mideleg bit 1,
// readback-verified against the zero-write forced set), install a
// vectored stvec over the two-slot jal table in stvm_trap.S with a
// stvec readback proving MODE=1 and BASE == the table address, open
// the address space with one PMP NAPOT entry, clear mie and
// mstatus.MIE, then drop to S-mode. Each trap stub records its own
// hardware entry pc (BASE + 4*cause) in s_regs[7], so the landing
// address is measured, not assumed.
//
// Phase A (exception): execute the 4-byte illegal word 0xffffffff at
// a labeled site. The trap must enter at BASE (slot 0) with
// scause = 0x2 and sepc exactly at the site; the handler skips the
// word by advancing sepc by 4 and raises the done flag.
//
// Phase B (interrupt): pend mip.SSIP from S-mode with SIE off
// (stable readback), then set SIE. The pending SSI must trap exactly
// once, entering at BASE+4 (slot 1) with
// scause = 0x8000000000000001 and sepc at the interrupted nop; the
// handler clears SSIP so the level-triggered source fires once.
// A control with SIE on and nothing pending, and a quiet window at
// the end, prove no extra traps fire. M-mode trap count must stay 0.

#include "../uart.h"

#define SCAUSE_ILLEGAL_INST 0x2UL                // exception code 2
#define SCAUSE_S_SOFT      0x8000000000000001UL  // supervisor interrupt, SSI code

static volatile unsigned long m_regs[8];  // mscratch points here
static volatile unsigned long s_regs[8];  // sscratch points here
volatile unsigned long s_done;            // raised by the S-mode handler

extern void stvm_table(void);
extern void m_trap_entry(void);
void smode_main(void);

static unsigned long stvec_base;   // BASE read back from stvec in M-mode
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

// Drop from M-mode to S-mode at smode_main. Mirrors the working
// construction in src/smode: sret takes the target privilege from
// sstatus.SPP. Never returns.
static void drop_to_smode(void) {
    unsigned long v;

    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    v |= (1UL << 8);  // SPP = 1 (S-mode)
    __asm__ volatile("csrw sstatus, %0" :: "r"(v));
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    v = (v & ~(3UL << 11)) | (1UL << 11);  // MPP = 01
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
    unsigned long spins, site, expected, sipv;

    // Arm the supervisor software interrupt in sie; SIE in sstatus is
    // still off, so nothing can fire until the phases set it.
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

    // Phase A: the synchronous exception. SIE stays off so nothing
    // else can interleave. The illegal word 0xffffffff has bits[1:0]
    // = 11 (a 32-bit instruction) and an unmapped major opcode, so it
    // traps as an illegal instruction of length 4.
    __asm__ volatile("csrci sstatus, 2");
    s_done = 0;
    __asm__ volatile(
        "la %0, 1f\n"
        "1: .word 0xffffffff\n"  // the known illegal word, at `site`
        : "=r"(site) :: "memory");
    spins = wait_for_done(10000000UL);
    uart_puts("exc: spins=");
    uart_put_dec(spins);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" entry=");
    uart_put_hex(s_regs[7]);
    uart_puts(" base=");
    uart_put_hex(stvec_base);
    uart_puts(" scause=");
    uart_put_hex(s_regs[2]);
    uart_puts(" sepc=");
    uart_put_hex(s_regs[3]);
    uart_puts(" site=");
    uart_put_hex(site);
    uart_puts(" sip-at-entry=");
    uart_put_hex(s_regs[4]);
    uart_puts("\n");
    check(spins < 10000000UL, "illegal-instruction trap never delivered");
    check(s_regs[0] == 1, "exception did not fire exactly once");
    check(s_regs[7] == stvec_base,
          "exception did not enter at BASE (slot 0)");
    check(s_regs[2] == SCAUSE_ILLEGAL_INST,
          "scause != illegal-instruction exception code 2");
    check(s_regs[3] == site, "sepc != illegal-word site address");
    check(m_regs[0] == 0, "M-mode trap fired during the exception phase");

    // Phase B: the supervisor software interrupt. Pend the interrupt's
    // own source (mip.SSIP, bit 1) with SIE off so the readback is
    // stable, then set SIE; the pending SSI is taken at the next
    // instruction boundary, so sepc must equal the address of the
    // nop, captured with an in-assembly local label.
    __asm__ volatile("csrci sstatus, 2");
    __asm__ volatile("csrs sip, %0" :: "r"(1UL << 1));  // pend SSIP
    __asm__ volatile("csrr %0, sip" : "=r"(sipv));
    uart_puts("int: sip-after-pend=");
    uart_put_hex(sipv);
    uart_puts("\n");
    check(sipv == 0x2, "sip readback != 0x2 after pending SSIP");
    s_done = 0;
    __asm__ volatile(
        "csrs sstatus, 2\n"   // sstatus.SIE = 1
        "1: nop\n"            // the pending SSI lands here
        "la %0, 1b\n"
        : "=r"(expected) :: "memory");
    spins = wait_for_done(10000000UL);
    __asm__ volatile("csrci sstatus, 2");  // SIE off before the checks
    uart_puts("int: spins=");
    uart_put_dec(spins);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" entry=");
    uart_put_hex(s_regs[7]);
    uart_puts(" base+4=");
    uart_put_hex(stvec_base + 4);
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
    check(s_regs[0] == 2, "SSI did not fire exactly once (total traps != 2)");
    check(s_regs[7] == stvec_base + 4,
          "SSI did not enter at BASE+4 (slot 1)");
    check(s_regs[2] == SCAUSE_S_SOFT,
          "scause != supervisor software interrupt");
    check(s_regs[3] == expected,
          "sepc != interrupted-instruction address");

    // The handler cleared SSIP: sip must read 0 now, and the
    // unexpected-exception marker slot must still be 0 (the BSS
    // clear zeroed s_regs before the drop).
    __asm__ volatile("csrr %0, sip" : "=r"(sipv));
    uart_puts("int: sip-after-handler=");
    uart_put_hex(sipv);
    uart_puts(" marker=");
    uart_put_hex(s_regs[6]);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");
    check(sipv == 0, "sip not cleared by the S-mode handler");
    check(s_regs[6] == 0, "unexpected exception code seen");
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
    check(s_regs[0] == 2 && m_regs[0] == 0,
          "trap fired during the quiet window");

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

static void csr_write_mideleg(unsigned long v) {
    __asm__ volatile("csrw mideleg, %0" :: "r"(v));
}

int main(void) {
    unsigned long rd_zero, rd, v, boot_medeleg, boot_mideleg;

    uart_init();
    uart_puts("stvec-vectored-mode: exception-at-BASE vs interrupt-at-BASE+4*cause test\n");

    // M-mode trap handler: direct-mode mtvec, mscratch at m_regs.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // Boot-time delegation values, for the record.
    __asm__ volatile("csrr %0, medeleg" : "=r"(boot_medeleg));
    boot_mideleg = csr_read_mideleg();
    uart_puts("boot: medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts(" mideleg=");
    uart_put_hex(boot_mideleg);
    uart_puts("\n");

    // Delegate ONLY the illegal-instruction exception (medeleg bit
    // 2). Write zero first and read back, then write bit 2 and
    // require the readback to equal the forced set with only bit 2
    // added.
    __asm__ volatile("csrw medeleg, %0" :: "r"(0UL));
    __asm__ volatile("csrr %0, medeleg" : "=r"(rd_zero));
    __asm__ volatile("csrw medeleg, %0" :: "r"(1UL << 2));
    __asm__ volatile("csrr %0, medeleg" : "=r"(rd));
    uart_puts("medeleg: write=0x0 readback=");
    uart_put_hex(rd_zero);
    uart_puts(" write=0x4 readback=");
    uart_put_hex(rd);
    uart_puts("\n");
    check(rd == (rd_zero | (1UL << 2)),
          "medeleg write changed more than bit 2");

    // Delegate ONLY the supervisor software interrupt (mideleg bit
    // 1). Same zero-write-then-selective-write construction; the
    // zero-write readback exposes the forced set this hart ORs in
    // (hypervisor interrupt bits on QEMU).
    csr_write_mideleg(0);
    rd_zero = csr_read_mideleg();
    uart_puts("mideleg: write=0x0 readback=");
    uart_put_hex(rd_zero);
    uart_puts("\n");
    csr_write_mideleg(rd_zero | (1UL << 1));
    rd = csr_read_mideleg();
    uart_puts("mideleg: write readback=");
    uart_put_hex(rd);
    uart_puts("\n");
    check(rd == (rd_zero | (1UL << 1)),
          "mideleg write changed more than bit 1");
    check((rd & (1UL << 9)) == 0,
          "supervisor external interrupt delegated (bit 9 set)");

    // Install stvec in vectored mode (MODE=1) over the two-slot table.
    // Read back and require the mode bits and the BASE address.
    __asm__ volatile("la t0, stvm_table\n"
                     "ori t0, t0, 1\n"
                     "csrw stvec, t0" ::: "t0");
    __asm__ volatile("csrr %0, stvec" : "=r"(v));
    uart_puts("stvec: written=");
    uart_put_hex((unsigned long)stvm_table | 1);
    uart_puts(" readback=");
    uart_put_hex(v);
    uart_puts("\n");
    check((v & 3UL) == 1, "stvec MODE did not read back 1 (vectored)");
    check((v & ~3UL) == (unsigned long)stvm_table,
          "stvec BASE != vector table address");
    stvec_base = v & ~3UL;

    // S-mode trap vector data and the done-flag address; PMP opening
    // the whole address space to S-mode (lower modes default-deny).
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_regs));
    s_regs[5] = (unsigned long)&s_done;
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // Disarm every M-mode interrupt enable: mie clear and
    // mstatus.MIE clear, so no M-mode trap can fire during the run.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrc mstatus, %0" :: "r"(1UL << 3));  // MIE off

    // Start from a clean pending state.
    __asm__ volatile("csrc mip, %0" :: "r"(1UL << 1));  // SSIP clear

    // Drop to S-mode; the test runs in smode_main.
    drop_to_smode();
}
