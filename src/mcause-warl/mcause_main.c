// mcause_main.c: mcause WARL (write-any/read-legal) check
// (backlog item 148).
//
// Mechanism under test: the mcause CSR is write-any/read-legal
// (WARL), meaning a write is always accepted and the readback is a
// legal value. On this hart (QEMU 8.2.2, virt machine) every one
// of the 64 bits is software-writable: writing all-ones reads back
// all-ones, writing zero reads back zero. The trap mechanism
// overwrites mcause on every trap regardless of what software
// wrote last.
//
// Sequence under test:
//   1. Boot: mcause reads 0. Install the trap handler (direct-mode
//      mtvec, mscratch scratch area) and read mtvec back to
//      confirm the handler took.
//   2. Issue an M-mode ecall. The handler records mcause and bumps
//      the trap counter; it resumes past the 4-byte ecall. The
//      ecall cause is 11 (environment call from M-mode). Require
//      trap_count == 1, the handler-recorded cause == 11, and an
//      independent csrr read of mcause == 11.
//   3. csrw all-ones (0xFFFFFFFFFFFFFFFF) to mcause. Require the
//      readback to equal all-ones and trap_count to still be 1
//      (the write fired no trap; the value took).
//   4. csrw 0 to mcause. Require the readback to equal 0 (the write
//      took; no bits are read-only).
//   5. Issue a second ecall. Require trap_count == 2 and the
//      readback mcause == 11, proving trap entry overwrites the
//      register regardless of the last software write.
//
// No interrupt source is ever armed and mstatus.MIE stays clear,
// so the only traps that can ever fire are the two deliberate
// ecalls, and the readback values are the ground truth for the
// software-write behavior.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.

#include "../uart.h"

#define ECALL_MCAUSE   11UL
#define MCAUSE_MASK    ~0UL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final RESULT line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

static volatile unsigned long mca_regs[8];  // trap scratch, mscratch points here
static volatile unsigned long trap_count = 0;
static volatile unsigned long last_mcause = 0;
static volatile unsigned long last_mepc = 0;

static unsigned long read_mcause(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mcause" : "=r"(v));
    return v;
}

static void write_mcause(unsigned long v) {
    __asm__ volatile("csrw mcause, %0" : : "r"(v));
}

// Called from the asm trap entry. Records the cause, counts the
// trap, and resumes past the 4-byte ecall so the run continues.
// Any trap that is not the deliberate M-mode ecall is unexpected:
// the hart is parked (never returns to the asm entry), which the
// harness observes as the timeout exit status, i.e. FAIL.
void mca_c_handle(void) {
    unsigned long cause = mca_regs[2];
    trap_count++;
    last_mcause = cause;
    last_mepc = mca_regs[3];
    if (cause == ECALL_MCAUSE) {
        // ecall is always a 4-byte instruction; resume past it by
        // writing the mepc CSR, not the saved copy.
        unsigned long epc;
        __asm__ volatile("csrr %0, mepc" : "=r"(epc));
        epc += 4;
        __asm__ volatile("csrw mepc, %0" : : "r"(epc));
        return;
    }
    for (;;)
        __asm__ volatile("wfi");
}

extern void mcause_trap_entry(void);

// Poll with a spin budget until the counter reaches want, so a
// broken delivery is a FAIL, not a hang.
static unsigned long wait_for_trap(unsigned long want, unsigned long budget) {
    unsigned long spins = 0;
    while (trap_count < want && spins < budget)
        spins++;
    return spins;
}

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// Prints one write/last-cause/readback triple: the software write
// value, the cause the last trap reported, and what mcause reads
// back after the write. These triples are the evidence for which
// bits of mcause are software-writable.
static void print_triple(const char *tag, unsigned long written,
                         unsigned long last, unsigned long readback) {
    uart_puts(tag);
    uart_puts(": write=");
    uart_put_hex(written);
    uart_puts(" last-cause=");
    uart_put_hex(last);
    uart_puts(" readback=");
    uart_put_hex(readback);
    uart_puts("\n");
}

int main(void) {
    unsigned long v, spins;

    uart_init();
    uart_puts("mcause-warl: mcause write-any/read-legal check\n");

    // 1. Boot state: mcause reads 0 before anything runs.
    v = read_mcause();
    uart_puts("boot: mcause=");
    uart_put_hex(v);
    uart_puts("\n");
    check(v == 0, "mcause nonzero at boot");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the scratch area. No interrupt source is armed and MIE
    // stays clear, so only the deliberate ecalls can trap.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mcause_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mca_regs));
    {
        unsigned long tv;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        uart_puts("trap: mtvec=");
        uart_put_hex(tv);
        uart_puts("\n");
        check((tv & ~3UL) == (unsigned long)mcause_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
    }

    // 2. First deliberate trap: an M-mode ecall. mcause must report
    // 11 both in the handler's record and in an independent read.
    __asm__ volatile("ecall");
    spins = wait_for_trap(1, 10000000UL);
    check(spins < 10000000UL, "first ecall did not trap");
    uart_puts("trap1: handler-cause=");
    uart_put_hex(last_mcause);
    uart_puts(" mepc=");
    uart_put_hex(last_mepc);
    uart_puts(" count=");
    uart_put_dec(trap_count);
    uart_puts("\n");
    check(last_mcause == ECALL_MCAUSE, "handler cause != 11 after ecall");
    check(trap_count == 1, "trap_count != 1 after first ecall");
    v = read_mcause();
    uart_puts("trap1: mcause-readback=");
    uart_put_hex(v);
    uart_puts(" (expect 0xb)\n");
    check(v == ECALL_MCAUSE, "mcause readback != 11 after ecall");

    // 3. Write all-ones to mcause. The readback must equal
    // all-ones (the write took), and no trap may have fired from
    // the write itself.
    write_mcause(MCAUSE_MASK);
    v = read_mcause();
    print_triple("write-ones", MCAUSE_MASK, last_mcause, v);
    check(v == MCAUSE_MASK, "mcause readback != all-ones after write");
    check(trap_count == 1, "trap fired from the all-ones write");

    // 4. Write zero to mcause. The readback must equal zero: no
    // bits are read-only.
    write_mcause(0UL);
    v = read_mcause();
    print_triple("write-zero", 0UL, last_mcause, v);
    check(v == 0UL, "mcause readback != 0 after write");
    check(trap_count == 1, "trap fired from the zero write");

    // 5. Second deliberate trap: trap entry must overwrite the
    // software-written value and report the new cause.
    __asm__ volatile("ecall");
    spins = wait_for_trap(2, 10000000UL);
    check(spins < 10000000UL, "second ecall did not trap");
    v = read_mcause();
    uart_puts("trap2: handler-cause=");
    uart_put_hex(last_mcause);
    uart_puts(" mcause-readback=");
    uart_put_hex(v);
    uart_puts(" count=");
    uart_put_dec(trap_count);
    uart_puts("\n");
    check(trap_count == 2, "trap_count != 2 after second ecall");
    check(last_mcause == ECALL_MCAUSE, "handler cause != 11 on second ecall");
    check(v == ECALL_MCAUSE, "mcause not updated by second trap");

    uart_puts("RESULT: ");
    uart_puts(fails == 0 ? "PASS" : "FAIL");
    uart_puts(" (traps=");
    uart_put_dec(trap_count);
    uart_puts(")\n");

    // Let the UART drain (TEMT: transmitter fully empty) before
    // touching the finisher device.
    while ((*UART0_LSR & LSR_TEMT) == 0)
        ;

    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    // FAIL: park the hart without touching the finisher device. The
    // harness runs QEMU under timeout, so a FAIL is observable as
    // the timeout exit status (124) as well as the RESULT line.
    for (;;)
        __asm__ volatile("wfi");
}
