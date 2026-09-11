// mili_main.c: medeleg bit-2 illegal-instruction trap destination
// switch (backlog item "riscv medeleg-illegal-inst-route").
//
// Exactly one mechanism is under test: the destination of an illegal
// instruction trap raised in S-mode as medeleg bit 2 flips. Phase 1
// runs with medeleg written to 0: the illegal instruction from
// S-mode must trap in M-mode with mcause = 2 (illegal instruction)
// and mepc exactly at the illegal word. Phase 2 runs with medeleg
// bit 2 set: the same illegal word must trap in S-mode with
// scause = 2 and sepc exactly at the illegal word. The trap counts
// per mode per phase and the medeleg write/readback values are
// published over UART.
//
// The illegal encoding is the word 0xffffffff at a labeled site.
// All 32 bits set is not a valid RV64 instruction in any extension
// (bits 1:0 are 11, so it decodes as a 32-bit instruction, and no
// opcode pattern covers all-ones), so the hart raises the illegal
// instruction trap synchronously at exactly that word.
//
// Sequence:
//   M-mode: install the trap handlers, record the boot medeleg,
//   write 0 to medeleg and read back the actual configuration
//   (bit 2 verified clear), install stvec/sscratch, open the whole
//   address space to S-mode with one PMP NAPOT entry, clear mie and
//   mstatus.MIE, arm the continuation in m_regs[7], then drop to
//   S-mode.
//   S-mode phase 1: capture the illegal-word address with an
//   in-assembly label and execute the illegal word. medeleg bit 2
//   is clear, so the trap lands in M-mode; the M-mode handler
//   records mcause/mepc/mstatus/mtval, verifies the trap came from
//   S-mode, and redirects mepc to phase2_mmode.
//   M-mode phase 2: clear the continuation, set medeleg bit 2
//   (keeping whatever read-modify state the CSR holds), require
//   the readback to have bit 2 set, drop to S-mode.
//   S-mode phase 2: capture the illegal-word address, execute it;
//   the S-mode handler records scause/sepc/sstatus/stval, skips the
//   word, and resumes the payload, which prints every measured
//   value, runs the 13 checks, takes a quiet window, and prints
//   the verdict. All interrupt enables stay clear for the whole
//   run, so no interrupt of either kind can fire.

#include "../uart.h"

#define SCAUSE_ILLEGAL_INST 2UL  // illegal instruction
#define MCAUSE_ILLEGAL_INST 2UL  // same cause code when taken in M-mode

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final RESULT line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

static volatile unsigned long m_regs[10];  // mscratch points here
static volatile unsigned long s_regs[10];  // sscratch points here
volatile unsigned long s_done;             // raised by the S-mode handler

static unsigned long p1_ill_addr;     // phase-1 illegal-word address (S-mode)
static unsigned long p2_ill_addr;     // phase-2 illegal-word address (S-mode)
static unsigned long boot_medeleg;    // medeleg at boot
static unsigned long p1_medeleg_rb;   // phase-1 medeleg readback
static unsigned long p2_medeleg_rb;   // phase-2 medeleg readback
static unsigned long p1_m_traps;      // M-mode trap count after phase 1
static unsigned long p1_s_traps;      // S-mode trap count after phase 1

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

static void csr_write_medeleg(unsigned long v) {
    __asm__ volatile("csrw medeleg, %0" :: "r"(v));
}

static unsigned long csr_read_medeleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, medeleg" : "=r"(v));
    return v;
}

extern void m_trap_entry(void);
extern void s_trap_entry(void);
void smode_phase1(void);
void smode_phase2(void);
void phase2_mmode(void);

// Drop from M-mode to S-mode at the given entry point. sret takes
// the target privilege from sstatus.SPP; mstatus.MPP is set to
// S-mode for a consistent view. Never returns.
static void drop_to_smode(void (*entry)(void)) {
    unsigned long v;

    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    v |= (1UL << 8);  // SPP = 1 (S-mode)
    __asm__ volatile("csrw sstatus, %0" :: "r"(v));
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    v = (v & ~(3UL << 11)) | (1UL << 11);  // MPP = 01, consistent view
    __asm__ volatile("csrw mstatus, %0" :: "r"(v));
    __asm__ volatile("csrw sepc, %0\n"
                     "sret" :: "r"((unsigned long)entry) : "memory");
    for (;;)
        __asm__ volatile("wfi");
}

// Phase-1 S-mode payload: capture the illegal-word address with an
// in-assembly forward label BEFORE it executes, then execute the
// word. medeleg bit 2 is clear, so the trap lands in M-mode; the
// M-mode handler redirects to phase2_mmode instead of resuming
// here. The slot address is passed as an operand so the compiler
// sees the reference and emits the variable's storage.
void smode_phase1(void) {
    unsigned long *slot = &p1_ill_addr;
    __asm__ volatile("la t0, 1f\n"
                     "sd t0, 0(%0)\n"
                     "1: .word 0xffffffff\n"
                     :: "r"(slot) : "t0", "memory");

    for (;;)  // unreachable: the M-mode handler never resumes here
        __asm__ volatile("wfi");
}

// M-mode phase 2, entered by the M-mode trap handler redirecting
// mepc here after the phase-1 illegal instruction. The continuation
// is consumed so any further M-mode trap parks the hart.
void phase2_mmode(void) {
    unsigned long rb;

    m_regs[7] = 0;
    csr_write_medeleg(csr_read_medeleg() | (1UL << 2));
    rb = csr_read_medeleg();
    p2_medeleg_rb = rb;

    // Snapshot the phase-1 trap counts before phase 2 can move them.
    p1_m_traps = m_regs[0];
    p1_s_traps = s_regs[0];

    drop_to_smode(smode_phase2);
}

// Phase-2 S-mode payload: capture the illegal-word address, execute
// the word. medeleg bit 2 is set, so the trap lands in the S-mode
// handler, which records scause/sepc/sstatus/stval, skips the word,
// and sret's back here. Everything after the trap is reporting.
void smode_phase2(void) {
    unsigned long addr, h;

    __asm__ volatile("la %0, 1f\n"
                     "1: .word 0xffffffff\n"
                     : "=r"(addr));
    p2_ill_addr = addr;

    uart_puts("medeleg-illegal-inst-route: illegal-instruction trap destination switch test\n");
    uart_puts("boot: medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts("\n");
    uart_puts("phase1: medeleg-write=0x0 readback=");
    uart_put_hex(p1_medeleg_rb);
    uart_puts("\n");
    uart_puts("phase2: medeleg bit2 set readback=");
    uart_put_hex(p2_medeleg_rb);
    uart_puts("\n");

    uart_puts("p1: m_traps=");
    uart_put_dec(p1_m_traps);
    uart_puts(" mcause=");
    uart_put_hex(m_regs[2]);
    uart_puts(" mepc=");
    uart_put_hex(m_regs[3]);
    uart_puts(" expected=");
    uart_put_hex(p1_ill_addr);
    uart_puts(" mstatus_mpp=");
    uart_put_dec((m_regs[4] >> 11) & 3UL);
    uart_puts(" mtval=");
    uart_put_hex(m_regs[8]);
    uart_puts(" s_traps=");
    uart_put_dec(p1_s_traps);
    uart_puts("\n");

    uart_puts("p2: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" scause=");
    uart_put_hex(s_regs[2]);
    uart_puts(" sepc=");
    uart_put_hex(s_regs[3]);
    uart_puts(" expected=");
    uart_put_hex(p2_ill_addr);
    uart_puts(" sstatus_spp=");
    uart_put_dec((s_regs[4] >> 8) & 1UL);
    uart_puts(" stval=");
    uart_put_hex(s_regs[8]);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");

    // Phase-1 checks: medeleg bit 2 clear, the illegal instruction
    // trapped in M-mode.
    check((p1_medeleg_rb & (1UL << 2)) == 0,
          "phase-1 medeleg readback has bit 2 set");
    check(p1_m_traps == 1, "phase-1 M-mode trap did not fire exactly once");
    check(m_regs[2] == MCAUSE_ILLEGAL_INST,
          "phase-1 mcause != illegal instruction (2)");
    check(m_regs[3] == p1_ill_addr, "phase-1 mepc != illegal-word address");
    check(((m_regs[4] >> 11) & 3UL) == 1,
          "phase-1 trap did not arrive from S-mode (mstatus.MPP)");
    check(p1_s_traps == 0,
          "phase-1 S-mode trap fired with medeleg bit 2 clear");
    // Phase-2 checks: medeleg bit 2 set, the illegal instruction
    // trapped in S-mode.
    check((p2_medeleg_rb & (1UL << 2)) != 0,
          "phase-2 medeleg readback missing bit 2");
    check(s_regs[0] == 1, "phase-2 S-mode trap did not fire exactly once");
    check(s_regs[2] == SCAUSE_ILLEGAL_INST,
          "phase-2 scause != illegal instruction (2)");
    check(s_regs[3] == p2_ill_addr, "phase-2 sepc != illegal-word address");
    check(((s_regs[4] >> 8) & 1UL) == 1,
          "phase-2 trap did not arrive from S-mode (sstatus.SPP)");
    // Cross-phase check: no trap leaked into the wrong mode's
    // handler in either phase.
    check(m_regs[0] == 1 && s_regs[0] == 1,
          "trap leaked into the wrong mode's handler");

    // Quiet window: nothing pending, all enables clear; the counts
    // must not move.
    {
        unsigned long spins = 0;
        while (spins < 2000000UL)
            spins++;
    }
    uart_puts("quiet: m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts("\n");
    check(m_regs[0] == 1 && s_regs[0] == 1,
          "trap count moved during the quiet window");

    // FNV-1a over the verdict values, so the three runs can be
    // compared byte for byte.
    h = 0xcbf29ce484222325UL;
    {
        unsigned long vals[8] = {
            m_regs[0], m_regs[2], m_regs[3], s_regs[0],
            s_regs[2], s_regs[3], p1_medeleg_rb, p2_medeleg_rb
        };
        unsigned long i, b;
        for (i = 0; i < 8; i++)
            for (b = 0; b < 8; b++) {
                h ^= (vals[i] >> (b * 8)) & 0xffUL;
                h *= 0x100000001b3UL;
            }
    }
    uart_puts("checksum=");
    uart_put_hex(h);
    uart_puts("\n");

    // Drain the UART before the finisher shuts the machine down, so
    // the final RESULT line is never cut off.
    while (!(*UART0_LSR & LSR_TEMT))
        ;
    if (fails == 0) {
        uart_puts("RESULT: PASS (checks=");
        uart_put_dec(checks);
        uart_puts(")\n");
        while (!(*UART0_LSR & LSR_TEMT))
            ;
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    uart_puts("RESULT: FAIL (checks=");
    uart_put_dec(checks);
    uart_puts(" fails=");
    uart_put_dec(fails);
    uart_puts(")\n");
    uart_puts("done\n");
    while (!(*UART0_LSR & LSR_TEMT))
        ;
    for (;;)  // FAIL: park the hart so the harness sees a timeout
        __asm__ volatile("wfi");
}

int main(void) {
    uart_init();

    // M-mode trap handler: direct-mode mtvec, mscratch at m_regs.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // S-mode trap handler: direct-mode stvec, sscratch at s_regs,
    // done-flag address for the S-mode handler.
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_regs));
    s_regs[5] = (unsigned long)&s_done;

    // Boot-time medeleg, for the record.
    boot_medeleg = csr_read_medeleg();

    // Phase 1: write 0 and read back the actual configuration the
    // first illegal instruction runs under.
    csr_write_medeleg(0);
    p1_medeleg_rb = csr_read_medeleg();

    // Open the whole address space to S-mode (lower modes
    // default-deny) with one PMP NAPOT entry.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // Disarm every interrupt enable: mie clear and mstatus.MIE
    // clear. sie and sstatus.SIE are never set; the only traps in
    // this run are the two illegal instructions.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrc mstatus, %0" :: "r"(1UL << 3));  // MIE off

    // Arm the phase-2 continuation the M-mode handler jumps to after
    // recording the phase-1 illegal instruction.
    m_regs[7] = (unsigned long)phase2_mmode;

    // Drop to S-mode; phase 1 runs in smode_phase1.
    drop_to_smode(smode_phase1);
}
