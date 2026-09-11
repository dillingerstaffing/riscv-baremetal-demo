// sec_main.c: stval environment-call capture test
// (backlog item "riscv stval-ecall-capture").
//
// Exactly one mechanism is under test: what the hart writes to
// stval when it takes an environment-call trap. The privileged
// specification does not define a faulting address or word for
// environment calls the way it does for instruction, load, and
// store faults, so the recorded value is checked against zero,
// the value the hart writes when there is nothing to report. The
// M-mode setup delegates U-mode environment calls to S-mode with
// medeleg bit 8, installs an S-mode direct-mode stvec handler that
// records scause, sepc, and stval at entry, and drops to U-mode
// via sret with sstatus.SPP = 0. The U-mode payload issues one
// ecall at a labeled site. The handler records the trap, raises
// the done flag, skips the ecall, and sret's back to U-mode. The
// payload then publishes the scause/sepc/stval triple and asserts
// scause == 8 and stval == 0, with zero M-mode traps.
//
// Sequence:
//   M-mode: install the trap handlers, record the boot medeleg,
//   run the write/readback triple on medeleg bit 8 (U-mode
//   environment-call delegation) requiring the bit admitted and
//   nothing else set, install stvec/sscratch, open the whole
//   address space to lower modes with one PMP NAPOT entry, clear
//   mie and mstatus.MIE (no M-mode trap can fire), then sret to
//   U-mode with SPP = 0.
//   U-mode: capture the ecall's address with an in-assembly local
//   label, execute ecall. The delegated trap lands in the S-mode
//   handler, which records scause/sepc/stval, raises the done
//   flag, skips the 4-byte ecall, and sret's back to U-mode. The
//   payload prints every measured value, runs the checks, takes a
//   quiet window, and prints the verdict. All interrupt enables
//   stay clear for the whole run, so no interrupt of either kind
//   can fire.

#include "../uart.h"

#define MEDELEG_U_ECALL_BIT (1UL << 8)  // U-mode environment-call delegation

#define SCAUSE_U_ECALL 8UL              // environment call from U-mode

#define MSTATUS_MIE (1UL << 3)

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

static unsigned long csr_read_medeleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, medeleg" : "=r"(v));
    return v;
}

static void csr_write_medeleg(unsigned long v) {
    __asm__ volatile("csrw medeleg, %0" :: "r"(v));
}

static unsigned long read_mie(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mie" : "=r"(v));
    return v;
}

extern void m_trap_entry(void);
extern void s_trap_entry(void);
void u_payload(void);

// Saved M-mode readbacks, published from U-mode for the record.
static unsigned long rd_zero_saved, rd_8_saved, rd_forced8_saved;

static void print_pair(const char *tag, unsigned long written,
                       unsigned long readback) {
    uart_puts(tag);
    uart_puts(": write=");
    uart_put_hex(written);
    uart_puts(" readback=");
    uart_put_hex(readback);
    uart_puts("\n");
}

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
    __asm__ volatile("mv t0, %0\n"
                     "csrw sepc, t0\n"
                     "sret" :: "r"(entry) : "t0");
    for (;;)
        __asm__ volatile("wfi");
}

// Poll with a spin budget until the done flag is raised, so a
// missing trap is a FAIL, not a hang.
static unsigned long wait_for_done(unsigned long budget) {
    unsigned long spins = 0;
    while (s_done == 0 && spins < budget)
        spins++;
    return spins;
}

// FNV-1a 64-bit over the verdict record. Deterministic across runs
// because the record holds no timing-dependent fields.
static unsigned long record_checksum(void) {
    unsigned long h = 1469598103934665603UL;
    unsigned long words[8];
    int i, j;

    words[0] = rd_zero_saved;
    words[1] = rd_8_saved;
    words[2] = rd_forced8_saved;
    words[3] = s_regs[0];  // S-mode trap count
    words[4] = s_regs[2];  // scause
    words[5] = s_regs[3];  // sepc
    words[6] = s_regs[4];  // stval
    words[7] = m_regs[0];  // M-mode trap count
    for (i = 0; i < 8; i++)
        for (j = 0; j < 8; j++) {
            h ^= (words[i] >> (j * 8)) & 0xffUL;
            h *= 1099511628211UL;
        }
    return h;
}

void u_payload(void) {
    unsigned long spins, ecall_addr, checksum;

    // Capture the ecall's address with an in-assembly local label,
    // then issue the call. medeleg bit 8 is set, so the
    // environment-call trap lands in the S-mode handler, which
    // records scause/sepc/stval, raises the done flag, skips the
    // ecall, and sret's back here to the 2: label.
    __asm__ volatile("la %0, 1f\n"
                     "1: ecall\n"
                     "2:\n"
                     : "=r"(ecall_addr) :: "memory");
    spins = wait_for_done(10000000UL);

    uart_puts("stval-ecall-capture: U-mode environment-call stval capture test\n");
    print_pair("medeleg", 0, rd_zero_saved);
    print_pair("medeleg", MEDELEG_U_ECALL_BIT, rd_8_saved);
    print_pair("medeleg", rd_zero_saved | MEDELEG_U_ECALL_BIT,
               rd_forced8_saved);
    uart_puts("ecall: spins=");
    uart_put_dec(spins);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" scause=");
    uart_put_hex(s_regs[2]);
    uart_puts(" sepc=");
    uart_put_hex(s_regs[3]);
    uart_puts(" expected=");
    uart_put_hex(ecall_addr);
    uart_puts(" stval=");
    uart_put_hex(s_regs[4]);
    uart_puts(" m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");

    check(rd_8_saved == (rd_zero_saved | MEDELEG_U_ECALL_BIT),
          "medeleg bit 8 not admitted cleanly");
    check(spins < 10000000UL, "U-mode ecall trap never delivered");
    check(s_regs[0] == 1, "S-mode trap did not fire exactly once");
    check(s_regs[2] == SCAUSE_U_ECALL,
          "scause != environment call from U-mode (8)");
    check(s_regs[3] == ecall_addr,
          "sepc != ecall address");
    check(s_regs[4] == 0,
          "stval != 0 on environment call");
    check(m_regs[0] == 0, "M-mode trap fired during the run");

    // Quiet window: everything disabled, the counts must not move.
    {
        unsigned long i;
        for (i = 0; i < 2000000UL; i++)
            __asm__ volatile("" ::: "memory");
    }
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
    unsigned long boot_medeleg, rd_zero, rd_8, rd_forced8, mv;

    uart_init();
    uart_puts("stval-ecall-capture: setup\n");

    // M-mode trap handler: direct-mode mtvec, mscratch at m_regs.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // Boot-time delegation value, for the record.
    boot_medeleg = csr_read_medeleg();
    uart_puts("boot: medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts("\n");

    // The write/readback triple for medeleg bit 8 (0x100, U-mode
    // environment call). Write zero first: the readback exposes
    // the forced set. Then write bit 8 alone and require the
    // readback to equal the forced set with only bit 8 added: the
    // write must change nothing else, and bit 8 must be admitted
    // (this is the bit the backlog item needs to route the trap to
    // S-mode).
    csr_write_medeleg(0);
    rd_zero = csr_read_medeleg();
    rd_zero_saved = rd_zero;
    print_pair("medeleg", 0, rd_zero);

    csr_write_medeleg(MEDELEG_U_ECALL_BIT);
    rd_8 = csr_read_medeleg();
    rd_8_saved = rd_8;
    print_pair("medeleg", MEDELEG_U_ECALL_BIT, rd_8);

    csr_write_medeleg(rd_zero | MEDELEG_U_ECALL_BIT);
    rd_forced8 = csr_read_medeleg();
    rd_forced8_saved = rd_forced8;
    print_pair("medeleg", rd_zero | MEDELEG_U_ECALL_BIT, rd_forced8);

    // S-mode trap vector and scratch, PMP opening the whole address
    // space to lower modes (lower modes default-deny), and the
    // done-flag address for the S-mode handler.
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_regs));
    s_regs[5] = (unsigned long)&s_done;
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // Disarm every interrupt enable before the drop: mie clear (so
    // no M-mode trap can fire while below M-mode) and mstatus.MIE
    // clear. sie and sstatus.SIE are never set anywhere in this
    // run.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    mv = read_mie();
    uart_puts("mie: readback=");
    uart_put_hex(mv);
    uart_puts("\n");
    __asm__ volatile("csrc mstatus, %0" :: "r"(MSTATUS_MIE));

    // Drop to U-mode; the test runs in u_payload.
    drop_to_umode(u_payload);

    // Never reached.
    for (;;)
        __asm__ volatile("wfi");
}
