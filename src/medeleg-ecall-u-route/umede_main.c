// umede_main.c: medeleg bit-8 U-mode ecall route
// (backlog item "riscv medeleg-ecall-u-route").
//
// Exactly one mechanism is under test: that medeleg bit 8 routes a
// U-mode ecall to S-mode. M-mode writes 0x100 to medeleg, reads it
// back, then sret's to U-mode (SPP = 0) at a one-instruction payload
// that issues a single ecall. The S-mode handler must record
// exactly one trap with scause = 8 (environment call from U-mode),
// sepc at the ecall, and sstatus.SPP = 0; no M-mode trap may fire
// during the delegation phase. The S-mode reporter then prints every
// measured value, runs the delegation checks, takes a quiet window,
// arms the restore, and issues an ecall from S-mode; with medeleg
// bit 9 clear that trap lands in M-mode, whose handler writes the
// boot medeleg value back, records the readback, and resumes at
// m_finalize, which runs the restore checks, prints the checksum
// over the verdict values, and prints RESULT.
//
// Sequence:
//   M-mode: install the trap handlers, record the boot medeleg,
//   write 0x100 and require the readback to equal 0x100 (bit 8
//   verified set, nothing else set), open the whole address space
//   to lower modes with one PMP NAPOT entry, clear mie and
//   mstatus.MIE, then sret to U-mode with SPP = 0.
//   U-mode: capture the ecall address with an in-assembly label,
//   issue ecall. medeleg bit 8 is set, so the trap lands in the
//   S-mode handler, which records scause/sepc/sstatus and redirects
//   to s_report in S-mode (SPP = 1).
//   S-mode reporter: print everything, run the 7 delegation checks,
//   quiet window, arm restore_armed, capture the restore-ecall
//   address, issue ecall from S-mode (bit 9 clear, so M-mode takes
//   it). M-mode handler restores medeleg = boot value, records the
//   readback, resumes at m_finalize, which runs the 3 restore
//   checks, prints the FNV-1a checksum, and prints the verdict.
// All interrupt enables stay clear for the whole run, so no
// interrupt of either kind can fire.

#include "../uart.h"

#define SCAUSE_U_ECALL 8UL  // environment call from U-mode
#define MCAUSE_S_ECALL 9UL  // restore ecall arrives from S-mode

static volatile unsigned long m_regs[8];  // mscratch points here
static volatile unsigned long s_regs[8];  // sscratch points here
volatile unsigned long s_done;            // raised by the S-mode handler

// Globals the assembly trap entries reference by symbol name.
unsigned long restore_armed;    // 1 once the reporter arms the restore ecall
unsigned long boot_medeleg;     // medeleg at boot
unsigned long restore_rb;       // medeleg readback after the restore write

static unsigned long u_ecall_addr;      // U-mode ecall address
static unsigned long medeleg_rb;        // medeleg readback after writing 0x100
static unsigned long restore_ecall_addr;  // restore-ecall address (S-mode)

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
void m_finalize(void);
void s_report(void);

// Drop from M-mode to U-mode at the given entry point. sret takes
// the target privilege from sstatus.SPP (0 = U-mode); mstatus.MPP
// is cleared to U-mode for a consistent view. Never returns.
static void drop_to_umode(void (*entry)(void)) {
    unsigned long v;

    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    v &= ~(1UL << 8);  // SPP = 0 (U-mode)
    __asm__ volatile("csrw sstatus, %0" :: "r"(v));
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    v &= ~(3UL << 11);  // MPP = 00, consistent view
    __asm__ volatile("csrw mstatus, %0" :: "r"(v));
    __asm__ volatile("csrw sepc, %0\n"
                     "sret" :: "r"((unsigned long)entry) : "memory");
    for (;;)
        __asm__ volatile("wfi");
}

// One-instruction U-mode payload: capture the ecall address with an
// in-assembly forward label, then issue ecall. The S-mode handler
// redirects to s_report instead of resuming here, so the code after
// the ecall never runs; the wfi loop is only a backstop.
void umode_payload(void) {
    unsigned long *slot = &u_ecall_addr;
    __asm__ volatile(".option push\n"
                     ".option norvc\n"
                     "la t0, 1f\n"
                     "sd t0, 0(%0)\n"
                     "1: ecall\n"
                     ".option pop\n"
                     :: "r"(slot) : "t0", "memory");

    for (;;)
        __asm__ volatile("wfi");
}

// S-mode reporter, entered by the S-mode trap handler (SPP = 1).
// Prints every measured value, runs the delegation checks, takes a
// quiet window, arms the restore, and issues the restore ecall from
// S-mode so M-mode can write the boot medeleg back (lower modes
// cannot write medeleg).
void s_report(void) {

    uart_puts("medeleg-ecall-u-route: U-mode ecall route test\n");
    uart_puts("boot: medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts(" write=0x100 readback=");
    uart_put_hex(medeleg_rb);
    uart_puts("\n");

    uart_puts("u-ecall: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" scause=");
    uart_put_hex(s_regs[2]);
    uart_puts(" sepc=");
    uart_put_hex(s_regs[3]);
    uart_puts(" expected=");
    uart_put_hex(u_ecall_addr);
    uart_puts(" sstatus_spp=");
    uart_put_dec((s_regs[4] >> 8) & 1UL);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");

    // Delegation checks: exactly one S-mode trap, cause 8, sepc at
    // the ecall, arrived from U-mode, and no M-mode trap fired.
    check(medeleg_rb == 0x100,
          "medeleg readback after writing 0x100 != 0x100");
    check(s_regs[0] == 1, "S-mode trap did not fire exactly once");
    check(s_regs[2] == SCAUSE_U_ECALL,
          "scause != environment call from U-mode (8)");
    check(s_regs[3] == u_ecall_addr, "sepc != ecall address");
    check(((s_regs[4] >> 8) & 1UL) == 0,
          "trap did not arrive from U-mode (sstatus.SPP)");
    check(m_regs[0] == 0,
          "M-mode trap fired during the delegation phase");

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
    check(m_regs[0] == 0 && s_regs[0] == 1,
          "trap count moved during the quiet window");

    // Arm the restore: the ecall below is issued from S-mode with
    // medeleg bit 9 clear, so it traps in M-mode, whose handler
    // writes the boot medeleg value back and resumes at m_finalize.
    // The address is stored with an in-assembly sd BEFORE the ecall
    // executes: the M-mode handler never resumes here, so a C store
    // placed after the template could never run.
    restore_armed = 1;
    {
        unsigned long *slot = &restore_ecall_addr;
        __asm__ volatile(".option push\n"
                         ".option norvc\n"
                         "la t0, 1f\n"
                         "sd t0, 0(%0)\n"
                         "1: ecall\n"
                         ".option pop\n"
                         :: "r"(slot) : "t0", "memory");
    }

    for (;;)  // unreachable: the M-mode handler never resumes here
        __asm__ volatile("wfi");
}

// M-mode finalizer, entered by the M-mode trap handler (MPP =
// M-mode) after the restore ecall. Runs the restore checks, prints
// the checksum over the verdict values, and prints RESULT.
void m_finalize(void) {
    unsigned long h;

    if (m_regs[7] != 0) {
        // Premature M-mode trap: the U-mode ecall was not
        // delegated. Report the evidence; the checks below fail
        // honestly on the recorded (zero) S-mode values. Restore
        // medeleg here anyway so the machine is left as found.
        uart_puts("premature M-mode trap: mcause=");
        uart_put_hex(m_regs[2]);
        uart_puts(" mepc=");
        uart_put_hex(m_regs[3]);
        uart_puts(" (expected the U-mode ecall to be delegated)\n");
        fails++;
        csr_write_medeleg(boot_medeleg);
        restore_rb = csr_read_medeleg();
    }

    uart_puts("restore: m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" mcause=");
    uart_put_hex(m_regs[2]);
    uart_puts(" mepc=");
    uart_put_hex(m_regs[3]);
    uart_puts(" expected=");
    uart_put_hex(restore_ecall_addr);
    uart_puts(" medeleg_restored=");
    uart_put_hex(restore_rb);
    uart_puts("\n");

    // Restore checks: the restore ecall arrived from S-mode and the
    // boot medeleg value is back in place.
    check(m_regs[2] == MCAUSE_S_ECALL,
          "restore trap mcause != environment call from S-mode (9)");
    check(m_regs[3] == restore_ecall_addr,
          "restore trap mepc != restore-ecall address");
    check(restore_rb == boot_medeleg,
          "medeleg was not restored to the boot value");

    // FNV-1a over the verdict values, so the three runs can be
    // compared byte for byte. Every value is a link-time constant
    // or a hardware readback; nothing depends on host timing.
    h = 0xcbf29ce484222325UL;
    {
        unsigned long vals[9] = {
            s_regs[0], s_regs[2], s_regs[3], m_regs[0], m_regs[2],
            medeleg_rb, restore_rb, u_ecall_addr, restore_ecall_addr
        };
        unsigned long i, b;
        for (i = 0; i < 9; i++)
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

    // Boot-time medeleg, for the record and the end-of-run restore.
    boot_medeleg = csr_read_medeleg();

    // Delegate U-mode ecalls: write bit 8, read back. The run only
    // proceeds if the readback is exactly 0x100 (bit 8 verified
    // set, nothing else set).
    csr_write_medeleg(1UL << 8);
    medeleg_rb = csr_read_medeleg();

    // Open the whole address space to lower modes (they
    // default-deny) with one PMP NAPOT entry: the U-mode payload
    // must fetch its ecall, and the S-mode handler and reporter
    // must fetch and access everything they use.
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

    // Drop to U-mode; the payload issues one ecall.
    drop_to_umode(umode_payload);
}
