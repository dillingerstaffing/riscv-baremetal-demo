// seip_clear_main.c: mideleg bit-9 clear-and-restore test
// (backlog item "riscv mideleg-seip-clear").
//
// Exactly one mechanism is under test: clearing mideleg bit 9 after
// the supervisor external interrupt is pending must move the next
// delivery from S-mode to M-mode, and re-setting the bit must move
// it back. Sibling of src/mideleg-seip-route, which verified the
// set path only.
//
// Backlog corrections, stated honestly (two of them). First: the
// backlog line said "pend STIP via a sip write" but named
// mcause=0xb and scause=0x8000000000000009 with a supervisor
// external interrupt in mind; STIP is code 5
// (mcause 0x8000000000000005), so the line conflated the timer
// and external sources. This module follows the sibling and pends
// SEIP with an M-mode "csrs mip, 1<<9", the same recipe the
// sibling measured. Second: the backlog's mcause=0xb is wrong for
// the phase-B trap. The pending source is mip.SEIP (bit 9); when
// its delegation is cleared the trap is taken in M-mode but keeps
// the source's code, mcause=0x8000000000000009, measured. Code
// 0xb would be the separate machine-external source (mip.MEIP),
// which is never pending in this module. Delegation selects which
// privilege takes the trap, not the cause code.
//
// Source notes (measured, not assumed): on QEMU 8.2.2's virt hart
// mideleg is WARL with a legalized boot readback of 0x1444, and bit
// 9 (0x200, SEI) is in the delegable set: the write takes. The
// supervisor external interrupt's pending bit is mip.SEIP (bit 9);
// an M-mode "csrs mip, 1<<9" sets it and "csrc mip, 1<<9" clears it
// (measured by the sibling), while S-mode sip writes to bit 9 are
// dropped (measured by the shipped src/sip-seip-write module).
//
// Sequence:
//   M-mode main(): install the trap handlers, record boot mideleg
//   and boot mstatus, publish the write/readback triple for bit 9,
//   write 0 to medeleg, install stvec/sscratch, open the whole
//   address space to S-mode with one PMP NAPOT entry, grant the
//   counters via mcounteren, clear mie and mstatus.MIE, disarm the
//   machine timer (mtimecmp = all-ones), pend SEIP, point the
//   M-mode ecall trampoline at phase_b, drop to S-mode.
//   Phase A (S-mode, smode_main): replicate the sibling. SEIP
//   pending with SIE off shows 0 traps of either kind (control);
//   SIE set takes exactly one S-mode trap with
//   scause=0x8000000000000009, sepc at the interrupted instruction,
//   sip showing SEIP at handler entry, and 0 M-mode traps. Then
//   ecall back to M-mode: medeleg is 0, so the ecall traps in
//   M-mode with mcause=9, and the M-mode handler jumps to the
//   trampoline target instead of returning to S-mode.
//   Phase B (M-mode, phase_b): clear mideleg bit 9 (write 0x1444,
//   require readback 0x1444 with bit 9 clear), pend SEIP again
//   (readback 0x200), controls with the enables off and then with
//   mie.SEIE on but MIE off showing no trap, then set MIE:
//   exactly one M-mode trap with mcause=0x8000000000000009 (the
//   SEI source keeps code 9; delegation selects the taker, not the
//   code), mepc at the interrupted instruction, and 0 new S-mode
//   traps. The M-mode enable for the SEIP source is mie.SEIE
//   (bit 9), not mie.MEIE (bit 11, a separate source never
//   pending here). The M-mode handler masks MIE+MPIE
//   before mret (the MPIE clear is load-bearing: mret restores MIE
//   from MPIE, and the SEI stays pending). Then clear MEIE again
//   so delegation, not the enable mask, is the variable under
//   test, re-set mideleg bit 9 (with the readback on the record),
//   point the trampoline at phase_d, drop to S-mode.
//   Phase C (S-mode, smode_phase_c): with bit 9 set again, one
//   more pended SEI traps exactly once in S-mode (s_traps=2) with
//   0 new M-mode traps. Then ecall back to M-mode.
//   Phase D (M-mode, phase_d): clear the pending SEIP, verify the
//   enables are off, run a 2,000,000-cycle quiet window with the
//   counters frozen, restore mideleg and mstatus to their boot
//   readbacks bit-for-bit, publish the checksum, PASS, finisher
//   shutdown.

#include "../uart.h"
#include "../preempt/clint.h"

#define MIDELEG_BOOT 0x1444UL       // forced bits on QEMU 8.2.2 virt
#define MIDELEG_SEI_BIT (1UL << 9)  // supervisor external interrupt
#define MIP_SEIP (1UL << 9)         // pending bit under test
#define SIE_SEIE (1UL << 9)         // S-mode enable for the SEI
// M-mode enable for the SEIP source. Bit 9 of mie, the same bit
// number as sie.SEIE: sie is a restricted view of mie, so phase
// A's S-mode "csrs sie, SEIE" set this M-mode bit too. This is
// NOT mie.MEIE (bit 11): MEIE gates the separate machine-external
// source mip.MEIP, which is never pending in this module.
#define MIE_SEIE (1UL << 9)

#define SCAUSE_S_EXT 0x8000000000000009UL  // supervisor interrupt, SEI code
// The M-mode trap for the same SEI source keeps code 9: delegation
// selects which privilege takes the trap, not the cause code. The
// backlog's mcause=0xb was wrong (a second backlog typo, corrected
// here by measurement); 0xb would be the machine-external source,
// whose pending bit mip.MEIP is never set in this module.
#define MCAUSE_M_EXT 0x8000000000000009UL

#define SSTATUS_SIE  (1UL << 1)
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
volatile unsigned long m_ecall_target;    // M-mode ecall trampoline target

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
void smode_phase_c(void);
void phase_b(void);
void phase_d(void);

// Saved record, published from the phase that owns each value.
static unsigned long boot_mstatus_saved;
static unsigned long rd_zero_saved, rd_9_saved, rd_forced9_saved;
static unsigned long rd_clear_saved, rd_reset_saved;
static unsigned long pendA_saved, pendB_saved, pendC_saved;
static unsigned long sepcA_saved, sepcC_saved, mepcB_saved;

static void print_pair(const char *tag, unsigned long written,
                       unsigned long readback) {
    uart_puts(tag);
    uart_puts(": write=");
    uart_put_hex(written);
    uart_puts(" readback=");
    uart_put_hex(readback);
    uart_puts("\n");
}

// Drop from M-mode to S-mode at target. Mirrors the working
// construction in src/smode: sret takes the target privilege from
// sstatus.SPP. Never returns.
static void drop_to_smode(void (*target)(void)) {
    unsigned long v;

    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    v |= (1UL << 8);  // SPP = 1 (S-mode)
    __asm__ volatile("csrw sstatus, %0" :: "r"(v));
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    v = (v & ~(3UL << 11)) | (1UL << 11);  // MPP = 01, consistent view
    __asm__ volatile("csrw mstatus, %0" :: "r"(v));
    __asm__ volatile("csrw sepc, %0\n"
                     "sret" :: "r"((unsigned long)target) : "memory");
    for (;;)
        __asm__ volatile("wfi");
}

// S-mode ecall with medeleg=0 traps in M-mode (mcause=9); the M-mode
// handler jumps to m_ecall_target instead of returning to S-mode.
// Never returns to the caller.
static void ecall_to_mmode(void) {
    __asm__ volatile("ecall" ::: "memory");
    for (;;)
        __asm__ volatile("wfi");
}

// Poll with a spin budget until the S-mode done flag is raised, so a
// missing trap is a FAIL, not a hang.
static unsigned long wait_for_done(unsigned long budget) {
    unsigned long spins = 0;
    while (s_done == 0 && spins < budget)
        spins++;
    return spins;
}

// Poll with a spin budget until an M-mode interrupt trap is
// recorded, so a missing trap is a FAIL, not a hang.
static unsigned long wait_for_mtrap(unsigned long budget) {
    unsigned long spins = 0;
    while (m_regs[0] == 0 && spins < budget)
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
    unsigned long words[16];
    int i, j;

    words[0] = rd_zero_saved;
    words[1] = rd_9_saved;
    words[2] = rd_forced9_saved;
    words[3] = rd_clear_saved;
    words[4] = rd_reset_saved;
    words[5] = pendA_saved;
    words[6] = pendB_saved;
    words[7] = pendC_saved;
    words[8] = s_regs[0];   // S-mode trap count (2)
    words[9] = m_regs[0];   // M-mode interrupt trap count (1)
    words[10] = m_regs[7];  // ecall count (2)
    words[11] = s_regs[2];  // scause of the S-mode traps
    words[12] = m_regs[2];  // mcause of the M-mode trap
    words[13] = sepcA_saved;
    words[14] = sepcC_saved;
    words[15] = mepcB_saved;
    for (i = 0; i < 16; i++)
        for (j = 0; j < 8; j++) {
            h ^= (words[i] >> (j * 8)) & 0xffUL;
            h *= 1099511628211UL;
        }
    return h;
}

// Phase A (S-mode): replicate the sibling's set-path result as the
// control. Then ecall back to M-mode, where the handler jumps to
// phase_b.
void smode_main(void) {
    unsigned long spins, expected, siev;

    __asm__ volatile("csrci sstatus, 2");  // SIE off, explicit
    __asm__ volatile("csrs sie, %0" :: "r"(SIE_SEIE));
    __asm__ volatile("csrr %0, sie" : "=r"(siev));
    uart_puts("phaseA: sie=");
    uart_put_hex(siev);
    uart_puts("\n");
    check((siev & SIE_SEIE) != 0, "phaseA: sie.SEIE did not take");

    // Control: SEIP pending, SEIE enabled, but SIE off. The
    // interrupt is masked; no trap of either kind may arrive.
    s_done = 0;
    quiet_poll(1000000UL);
    uart_puts("phaseA-control: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");
    check(s_regs[0] == 0 && m_regs[0] == 0,
          "phaseA: trap arrived with SEIP pending but SIE off");

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
    uart_puts("phaseA-sei: spins=");
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
    check(spins < 10000000UL, "phaseA: supervisor external interrupt never delivered");
    check(s_regs[0] == 1, "phaseA: S-mode trap did not fire exactly once");
    check(s_regs[2] == SCAUSE_S_EXT, "phaseA: scause != supervisor external interrupt");
    check(s_regs[3] == expected, "phaseA: sepc != interrupted-instruction address");
    check((s_regs[4] & MIP_SEIP) != 0, "phaseA: sip at handler entry did not show SEIP");
    check(m_regs[0] == 0, "phaseA: M-mode trap fired during phase A");
    sepcA_saved = s_regs[3];

    ecall_to_mmode();  // noreturn; M-mode handler jumps to phase_b
}

// Phase B (M-mode): the claim under test. Clear mideleg bit 9 and
// require the pended SEI to arrive in M-mode instead of S-mode.
void phase_b(void) {
    unsigned long rd, pend_rb, spins, expected, mv;

    // Clear mideleg bit 9: write the forced set without the bit and
    // require the readback to equal it with bit 9 clear.
    csr_write_mideleg(MIDELEG_BOOT);
    rd = csr_read_mideleg();
    rd_clear_saved = rd;
    print_pair("phaseB-mideleg-clear", MIDELEG_BOOT, rd);
    check(rd == MIDELEG_BOOT, "phaseB: mideleg clear did not read back 0x1444");
    check((rd & MIDELEG_SEI_BIT) == 0, "phaseB: mideleg bit 9 still set after clear");

    // Pend SEIP again (still pending from phase A; the csrs is
    // idempotent, the readback is the claim).
    __asm__ volatile("csrs mip, %0" :: "r"(MIP_SEIP));
    pend_rb = read_mip();
    pendB_saved = pend_rb;
    uart_puts("phaseB-pend: mip-after-csrs=");
    uart_put_hex(pend_rb);
    uart_puts("\n");
    check(pend_rb == MIP_SEIP, "phaseB: mip readback != 0x200 after pending SEIP");

    // Enable the M-mode path for the SEIP source: mie.SEIE (bit
    // 9). Note mie bit 9 is still set from phase A's "csrs sie,
    // SEIE" (sie is a restricted view of mie), so clear mie to 0
    // first for a clean, self-contained phase-B setup.
    __asm__ volatile("csrc mie, %0" :: "r"(MIE_SEIE));
    check(read_mie() == 0, "phaseB: mie did not clear to 0");
    // Control 1: delegation cleared, enables off, SEIP pending.
    // No trap of either kind may arrive.
    quiet_poll(1000000UL);
    uart_puts("phaseB-control1: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");
    check(s_regs[0] == 1 && m_regs[0] == 0,
          "phaseB: trap arrived with delegation cleared and enables off");
    // Set the source enable; MIE stays off for control 2.
    __asm__ volatile("csrs mie, %0" :: "r"(MIE_SEIE));
    check(read_mie() == MIE_SEIE, "phaseB: mie.SEIE did not take on readback");
    quiet_poll(1000000UL);
    uart_puts("phaseB-control2: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");
    check(s_regs[0] == 1 && m_regs[0] == 0,
          "phaseB: trap arrived with SEIE on but MIE off");

    // Main: set MIE; with delegation cleared the pending SEI must
    // trap in M-mode at the next instruction boundary.
    __asm__ volatile(
        "csrs mstatus, 8\n"   // mstatus.MIE = 1
        "1: nop\n"            // the pending SEI lands here
        "la %0, 1b\n"
        : "=r"(expected) :: "memory");
    spins = wait_for_mtrap(10000000UL);
    uart_puts("phaseB-trap: spins=");
    uart_put_dec(spins);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" mcause=");
    uart_put_hex(m_regs[2]);
    uart_puts(" mepc=");
    uart_put_hex(m_regs[3]);
    uart_puts(" expected=");
    uart_put_hex(expected);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts("\n");
    check(spins < 10000000UL, "phaseB: machine external interrupt never delivered");
    check(m_regs[0] == 1, "phaseB: M-mode trap did not fire exactly once");
    check(m_regs[2] == MCAUSE_M_EXT, "phaseB: mcause != supervisor external interrupt");
    check(m_regs[3] == expected, "phaseB: mepc != interrupted-instruction address");
    check(s_regs[0] == 1, "phaseB: S-mode trap count moved during phase B");
    check(m_regs[7] == 1, "phaseB: ecall count != 1");
    mepcB_saved = m_regs[3];

    // Delegation, not the enable mask, is the variable under test:
    // clear the source enable again before the S-mode phase.
    __asm__ volatile("csrc mie, %0" :: "r"(MIE_SEIE));
    check(read_mie() == 0, "phaseB: mie did not clear on readback");

    // Re-set mideleg bit 9 before the S-mode phase: the pending
    // SEI must be delegated again for phase C.
    csr_write_mideleg(MIDELEG_BOOT | MIDELEG_SEI_BIT);
    mv = csr_read_mideleg();
    rd_reset_saved = mv;
    print_pair("phaseB-mideleg-reset", MIDELEG_BOOT | MIDELEG_SEI_BIT, mv);
    check(mv == (MIDELEG_BOOT | MIDELEG_SEI_BIT),
          "phaseB: mideleg re-set did not read back 0x1644");

    m_ecall_target = (unsigned long)phase_d;
    drop_to_smode(smode_phase_c);
    for (;;)
        __asm__ volatile("wfi");
}

// Phase C (S-mode): with bit 9 set again, one more pended SEI must
// trap exactly once in S-mode. Then ecall back to M-mode, where the
// handler jumps to phase_d.
void smode_phase_c(void) {
    unsigned long spins, expected, siev;

    // mideleg bit 9 was re-set in M-mode before this drop (see
    // phase_b's tail); here just verify delivery is back in S-mode.
    s_done = 0;
    __asm__ volatile("csrci sstatus, 2");  // SIE off, explicit
    __asm__ volatile("csrs sie, %0" :: "r"(SIE_SEIE));
    __asm__ volatile("csrr %0, sie" : "=r"(siev));
    uart_puts("phaseC: sie=");
    uart_put_hex(siev);
    uart_puts("\n");
    check((siev & SIE_SEIE) != 0, "phaseC: sie.SEIE did not take");

    __asm__ volatile(
        "csrs sstatus, 2\n"   // sstatus.SIE = 1
        "1: nop\n"            // the pending SEI lands here
        "la %0, 1b\n"
        : "=r"(expected) :: "memory");
    spins = wait_for_done(10000000UL);
    __asm__ volatile("csrci sstatus, 2");  // SIE off before the checks
    uart_puts("phaseC-sei: spins=");
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
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");
    check(spins < 10000000UL, "phaseC: supervisor external interrupt never delivered");
    check(s_regs[0] == 2, "phaseC: S-mode trap count != 2");
    check(s_regs[2] == SCAUSE_S_EXT, "phaseC: scause != supervisor external interrupt");
    check(s_regs[3] == expected, "phaseC: sepc != interrupted-instruction address");
    check((s_regs[4] & MIP_SEIP) != 0, "phaseC: sip at handler entry did not show SEIP");
    check(m_regs[0] == 1, "phaseC: M-mode trap count moved during phase C");
    sepcC_saved = s_regs[3];

    ecall_to_mmode();  // noreturn; M-mode handler jumps to phase_d
}

int main(void) {
    unsigned long boot_mideleg, rd_zero, rd_9, rd_forced9, pend_rb, mv;

    uart_init();
    uart_puts("mideleg-seip-clear: mideleg bit-9 clear moves a pended SEI from S-mode to M-mode\n");

    // Boot-time readbacks, for the record and the final restore.
    __asm__ volatile("csrr %0, mstatus" : "=r"(boot_mstatus_saved));
    uart_puts("boot: mstatus=");
    uart_put_hex(boot_mstatus_saved);
    uart_puts("\n");

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

    // The write/readback triple for mideleg bit 9 (0x200, SEI),
    // same as the sibling: the write takes on this hart.
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

    // No exceptions delegated: S-mode ecalls trap in M-mode, which
    // is how the S-mode phases hand control back for the next
    // phase.
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
    // readback is taken with MIE off so it is stable.
    __asm__ volatile("csrs mip, %0" :: "r"(MIP_SEIP));
    pend_rb = read_mip();
    pendA_saved = pend_rb;
    uart_puts("pend: mip-after-csrs=");
    uart_put_hex(pend_rb);
    uart_puts("\n");
    check(pend_rb == MIP_SEIP, "mip readback != 0x200 after pending SEIP");

    // The S-mode phases return to M-mode via ecall; the handler
    // jumps to the trampoline target. Phase A hands to phase_b.
    m_ecall_target = (unsigned long)phase_b;
    drop_to_smode(smode_main);

    // Never reached.
    for (;;)
        __asm__ volatile("wfi");
}

// Phase D (M-mode): clear the pending bit, run the quiet window,
// restore the boot CSRs bit-for-bit, publish the checksum, verdict.
void phase_d(void) {
    unsigned long mv, checksum, qs;

    // Pend SEIP one last time for the record (still pending from
    // the phases above; the csrs is idempotent, the readback is
    // the claim).
    __asm__ volatile("csrs mip, %0" :: "r"(MIP_SEIP));
    mv = read_mip();
    pendC_saved = mv;
    uart_puts("phaseD-pend: mip-after-csrs=");
    uart_put_hex(mv);
    uart_puts("\n");
    check(mv == MIP_SEIP, "phaseD: mip readback != 0x200");

    // Clear the pending SEIP from M-mode (S-mode never could) and
    // verify every enable is off.
    __asm__ volatile("csrc mip, %0" :: "r"(MIP_SEIP));
    check(read_mip() == 0, "phaseD: mip not clean after clearing SEIP");
    // Phase C's "csrs sie, SEIE" set mie bit 9 again through the
    // sie/mie alias; clear it before requiring a clean mie.
    __asm__ volatile("csrc mie, %0" :: "r"(MIE_SEIE));
    check(read_mie() == 0, "phaseD: mie not clear");
    __asm__ volatile("csrr %0, mstatus" : "=r"(mv));
    check((mv & MSTATUS_MIE) == 0, "phaseD: mstatus.MIE set");

    // Quiet window: 2,000,000 cycles with the enables cleared and
    // the pending bit gone. No counter may move.
    qs = read_cycle();
    while (read_cycle() - qs < 2000000UL)
        ;
    uart_puts("quiet: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" ecalls=");
    uart_put_dec(m_regs[7]);
    uart_puts("\n");
    check(s_regs[0] == 2 && m_regs[0] == 1 && m_regs[7] == 2,
          "phaseD: trap count moved during the quiet window");

    // Restore boot mideleg/mstatus bit-for-bit.
    csr_write_mideleg(MIDELEG_BOOT);
    mv = csr_read_mideleg();
    uart_puts("restore: mideleg=");
    uart_put_hex(mv);
    uart_puts("\n");
    check(mv == MIDELEG_BOOT, "phaseD: mideleg not restored to boot 0x1444");
    __asm__ volatile("csrw mstatus, %0" :: "r"(boot_mstatus_saved));
    __asm__ volatile("csrr %0, mstatus" : "=r"(mv));
    uart_puts("restore: mstatus=");
    uart_put_hex(mv);
    uart_puts(" boot=");
    uart_put_hex(boot_mstatus_saved);
    uart_puts("\n");
    check(mv == boot_mstatus_saved, "phaseD: mstatus not restored to boot value");

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
