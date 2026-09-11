// mede_main.c: medeleg bit-9 S-mode ecall destination switch
// (backlog item "riscv medeleg-ecall-destination").
//
// Exactly one mechanism is under test: the destination of a
// supervisor environment call as medeleg bit 9 flips. Phase 1 runs
// with medeleg zeroed: an ecall from S-mode must trap in M-mode
// with mcause = 9 (environment call from S-mode) and mepc at the
// ecall. Phase 2 runs with medeleg bit 9 set: the same ecall must
// trap in S-mode with scause = 9 and sepc at the ecall. The trap
// counts per mode per phase are published over UART.
//
// Cause-code note: the backlog item as written names mcause = 0xb
// for the phase-1 trap. 0xb is the environment call from M-mode;
// an ecall issued in S-mode traps with cause 9 (environment call
// from S-mode) whether or not it is delegated, which is what the
// RISC-V privileged specification assigns and what this run
// measures. The checks below assert 0x9, not 0xb.
//
// Sequence:
//   M-mode: install the trap handlers, record the boot medeleg,
//   write 0 to medeleg and read back 0 (bit 9 verified clear),
//   install stvec/sscratch, open the whole address space to S-mode
//   with one PMP NAPOT entry, clear mie and mstatus.MIE, arm the
//   continuation in m_regs[7], then drop to S-mode.
//   S-mode phase 1: capture the ecall address with an in-assembly
//   label and issue ecall. The M-mode handler records
//   mcause/mepc/mstatus, verifies the trap came from S-mode, and
//   redirects mepc to phase2_mmode.
//   M-mode phase 2: clear the continuation, write 0x200 to medeleg,
//   require the readback to have bit 9 set, drop to S-mode.
//   S-mode phase 2: capture the ecall address, issue ecall; the
//   S-mode handler records scause/sepc/sstatus, skips the ecall,
//   and resumes the payload, which prints every measured value,
//   runs the 14 checks, takes a quiet window, and prints the
//   verdict. All interrupt enables stay clear for the whole run,
//   so no interrupt of either kind can fire.

#include "../uart.h"

#define SCAUSE_S_ECALL 9UL  // environment call from S-mode
#define MCAUSE_S_ECALL 9UL  // same cause code when taken in M-mode

static volatile unsigned long m_regs[8];  // mscratch points here
static volatile unsigned long s_regs[8];  // sscratch points here
volatile unsigned long s_done;            // raised by the S-mode handler

static unsigned long p1_ecall_addr;   // phase-1 ecall address (S-mode)
static unsigned long p2_ecall_addr;   // phase-2 ecall address (S-mode)
static unsigned long boot_medeleg;    // medeleg at boot
static unsigned long p1_medeleg_rb;   // phase-1 medeleg readback
static unsigned long p2_medeleg_rb;   // phase-2 medeleg readback
static unsigned long p1_m_traps;      // M-mode trap count after phase 1
static unsigned long p1_s_traps;      // S-mode trap count after phase 1

static int fails = 0;

static void check(int cond, const char *msg) {
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

// Phase-1 S-mode payload: capture the ecall address with an
// in-assembly forward label (the code after the ecall never runs in
// S-mode, so the address must be taken before the trap), then issue
// ecall. medeleg is zero, so the trap lands in M-mode; the M-mode
// handler redirects to phase2_mmode instead of resuming here.
void smode_phase1(void) {
    // Capture the ecall address into p1_ecall_addr with an
    // in-assembly label BEFORE the ecall executes: the M-mode
    // handler redirects to phase2_mmode instead of resuming this
    // payload, so no C code placed after the ecall could ever run.
    // The slot address is passed as an operand so the compiler sees
    // the reference and emits the variable's storage.
    unsigned long *slot = &p1_ecall_addr;
    __asm__ volatile("la t0, 1f\n"
                     "sd t0, 0(%0)\n"
                     "1: ecall\n"
                     :: "r"(slot) : "t0", "memory");

    for (;;)  // unreachable: the M-mode handler never resumes here
        __asm__ volatile("wfi");
}

// M-mode phase 2, entered by the M-mode trap handler redirecting
// mepc here after the phase-1 ecall. The continuation is consumed
// so any further M-mode trap parks the hart.
void phase2_mmode(void) {
    unsigned long rb;

    m_regs[7] = 0;
    csr_write_medeleg(1UL << 9);
    rb = csr_read_medeleg();
    p2_medeleg_rb = rb;

    // Snapshot the phase-1 trap counts before phase 2 can move them.
    p1_m_traps = m_regs[0];
    p1_s_traps = s_regs[0];

    drop_to_smode(smode_phase2);
}

// Phase-2 S-mode payload: capture the ecall address, issue ecall.
// medeleg bit 9 is set, so the trap lands in the S-mode handler,
// which records scause/sepc/sstatus, skips the ecall, and sret's
// back here. Everything after the trap is reporting.
void smode_phase2(void) {
    unsigned long addr, h;

    __asm__ volatile("la %0, 1f\n"
                     "1: ecall\n"
                     : "=r"(addr));
    p2_ecall_addr = addr;

    uart_puts("medeleg-ecall-destination: S-mode ecall destination switch test\n");
    uart_puts("boot: medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts("\n");
    uart_puts("phase1: medeleg-write=0x0 readback=");
    uart_put_hex(p1_medeleg_rb);
    uart_puts("\n");
    uart_puts("phase2: medeleg-write=0x200 readback=");
    uart_put_hex(p2_medeleg_rb);
    uart_puts("\n");

    uart_puts("p1: m_traps=");
    uart_put_dec(p1_m_traps);
    uart_puts(" mcause=");
    uart_put_hex(m_regs[2]);
    uart_puts(" mepc=");
    uart_put_hex(m_regs[3]);
    uart_puts(" expected=");
    uart_put_hex(p1_ecall_addr);
    uart_puts(" mstatus_mpp=");
    uart_put_dec((m_regs[4] >> 11) & 3UL);
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
    uart_put_hex(p2_ecall_addr);
    uart_puts(" sstatus_spp=");
    uart_put_dec((s_regs[4] >> 8) & 1UL);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");

    // Phase-1 checks: medeleg zeroed, the ecall trapped in M-mode.
    check(p1_medeleg_rb == 0, "phase-1 medeleg readback != 0");
    check((p1_medeleg_rb & (1UL << 9)) == 0,
          "phase-1 medeleg readback has bit 9 set");
    check(p1_m_traps == 1, "phase-1 M-mode trap did not fire exactly once");
    check(m_regs[2] == MCAUSE_S_ECALL,
          "phase-1 mcause != environment call from S-mode (9)");
    check(m_regs[3] == p1_ecall_addr, "phase-1 mepc != ecall address");
    check(((m_regs[4] >> 11) & 3UL) == 1,
          "phase-1 trap did not arrive from S-mode (mstatus.MPP)");
    check(p1_s_traps == 0,
          "phase-1 S-mode trap fired with medeleg bit 9 clear");
    // Phase-2 checks: medeleg bit 9 set, the ecall trapped in S-mode.
    check((p2_medeleg_rb & (1UL << 9)) != 0,
          "phase-2 medeleg readback missing bit 9");
    check(s_regs[0] == 1, "phase-2 S-mode trap did not fire exactly once");
    check(s_regs[2] == SCAUSE_S_ECALL,
          "phase-2 scause != environment call from S-mode (9)");
    check(s_regs[3] == p2_ecall_addr, "phase-2 sepc != ecall address");
    check(((s_regs[4] >> 8) & 1UL) == 1,
          "phase-2 trap did not arrive from S-mode (sstatus.SPP)");
    // Cross-phase checks: no trap leaked into the other mode's
    // handler in either phase.
    check(s_regs[0] == 1 && m_regs[0] == 1,
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

    // Phase 1: delegate nothing. Write 0 and read back: the
    // delegation state the first ecall runs under.
    csr_write_medeleg(0);
    p1_medeleg_rb = csr_read_medeleg();

    // Open the whole address space to S-mode (lower modes
    // default-deny) with one PMP NAPOT entry.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // Disarm every interrupt enable: mie clear (so no M-mode trap
    // can fire while in S-mode) and mstatus.MIE clear. sie and
    // sstatus.SIE are never set; the only traps in this run are the
    // two ecalls.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrc mstatus, %0" :: "r"(1UL << 3));  // MIE off

    // Arm the phase-2 continuation the M-mode handler jumps to after
    // recording the phase-1 ecall.
    m_regs[7] = (unsigned long)phase2_mmode;

    // Drop to S-mode; phase 1 runs in smode_phase1.
    drop_to_smode(smode_phase1);
}
