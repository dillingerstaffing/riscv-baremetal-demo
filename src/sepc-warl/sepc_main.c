// sepc_main.c: sepc CSR software-write probe (backlog item 123).
//
// Mechanism under test: the privileged spec defines sepc as fully
// software-writable, so a csrw to sepc is accepted and the readback
// equals the written value for every XLEN bit. The trap mechanism
// overwrites sepc on every trap entry with the interrupted program
// counter, so a software-written value never survives a trap.
//
// Sequence under test:
//   1. Boot: sepc reads 0. Install the trap handler (direct-mode
//      mtvec, mscratch scratch area) and read mtvec back to confirm
//      the handler took.
//   2. csrw all-ones (0xFFFFFFFFFFFFFFFF) to sepc. Require the
//      readback to equal all-ones and publish the write/readback
//      pair.
//   3. csrw 0 to sepc. Require the readback to equal 0 and publish
//      the write/readback pair. After both writes the trap counter
//      must still be 0 (a CSR write firing a trap would be visible).
//   4. Issue a deliberate M-mode ecall. Trap entry into M-mode
//      writes mepc with the ecall pc; sepc is only written when a
//      trap is taken into S-mode, so sepc must still hold the
//      software-written 0 after the trap. The handler records
//      cause/sepc/mepc, bumps the trap counter, and resumes past
//      the 4-byte ecall. The ecall cause is 11 (environment call
//      from M-mode). Require trap_count == 1, the handler-recorded
//      mepc to equal the ecall pc, the handler-recorded sepc and
//      an independent csrr read of sepc to both still equal 0, and
//      the post-handler mepc readback to equal ecall pc + 4. This
//      keeps the module honest about what sepc is: the S-mode
//      exception pc register, not the M-mode one.
//
// The ecall pc is captured with an in-asm numeric local label
// ("la t0, 1f" before "1: ecall") so no C label-value construct is
// used. No interrupt source is ever armed and mstatus.MIE stays
// clear, so the only trap that can ever fire is the deliberate
// ecall; any other trap parks the hart (FAIL).
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.

#include "../uart.h"

#define ECALL_MCAUSE   11UL
#define SEPC_MASK      ~0UL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final RESULT line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

static volatile unsigned long sep_regs[8];  // trap scratch, mscratch points here
static volatile unsigned long trap_count = 0;
static volatile unsigned long last_cause = 0;
static volatile unsigned long last_sepc = 0;
static volatile unsigned long last_mepc = 0;

static unsigned long read_sepc(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, sepc" : "=r"(v));
    return v;
}

static void write_sepc(unsigned long v) {
    __asm__ volatile("csrw sepc, %0" : : "r"(v));
}

// Called from the asm trap entry. Records the cause, sepc, and
// mepc, counts the trap, and resumes past the 4-byte ecall so the
// run continues. Any trap that is not the deliberate M-mode ecall
// is unexpected: the hart is parked (never returns to the asm
// entry), which the harness observes as the timeout exit status,
// i.e. FAIL.
void sep_c_handle(void) {
    unsigned long cause = sep_regs[2];
    trap_count++;
    last_cause = cause;
    last_sepc = sep_regs[3];
    last_mepc = sep_regs[6];
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

extern void sepc_trap_entry(void);

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

// Prints one write/readback pair: the software write value and what
// sepc reads back after the write. These pairs are the evidence for
// which bits of sepc are software-writable.
static void print_pair(const char *tag, unsigned long written,
                       unsigned long readback) {
    uart_puts(tag);
    uart_puts(": write=");
    uart_put_hex(written);
    uart_puts(" readback=");
    uart_put_hex(readback);
    uart_puts("\n");
}

int main(void) {
    unsigned long v, spins, ecall_pc;

    uart_init();
    uart_puts("sepc-warl: sepc software-write probe\n");

    // 1. Boot state: sepc reads 0 before anything runs.
    v = read_sepc();
    uart_puts("boot: sepc=");
    uart_put_hex(v);
    uart_puts("\n");
    check(v == 0, "sepc nonzero at boot");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the scratch area. No interrupt source is armed and MIE
    // stays clear, so only the deliberate ecall can trap.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)sepc_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)sep_regs));
    {
        unsigned long tv;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        uart_puts("trap: mtvec=");
        uart_put_hex(tv);
        uart_puts("\n");
        check((tv & ~3UL) == (unsigned long)sepc_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
    }

    // 2. Write all-ones to sepc. The readback must equal all-ones:
    // the write took, every XLEN bit stuck.
    write_sepc(SEPC_MASK);
    v = read_sepc();
    print_pair("write-ones", SEPC_MASK, v);
    check(v == SEPC_MASK, "sepc readback != all-ones after write");
    check(trap_count == 0, "trap fired from the all-ones write");

    // 3. Write zero to sepc. The readback must equal zero: no bits
    // are read-only.
    write_sepc(0UL);
    v = read_sepc();
    print_pair("write-zero", 0UL, v);
    check(v == 0UL, "sepc readback != 0 after write");
    check(trap_count == 0, "trap fired from the zero write");

    // 4. Deliberate trap: an M-mode ecall. Trap entry into M-mode
    // writes mepc with the ecall pc; sepc (the S-mode exception pc
    // register) must be left alone, still holding the
    // software-written 0. Capture the ecall pc with an in-asm
    // numeric local label (no C label-value construct).
    __asm__ volatile("la t0, 1f\n\t"
                     "sd t0, %0\n\t"
                     "1: ecall\n\t"
                     "2:\n"
                     : "=m"(ecall_pc)
                     :
                     : "t0", "memory");
    spins = wait_for_trap(1, 10000000UL);
    check(spins < 10000000UL, "deliberate ecall did not trap");
    uart_puts("trap: ecall-pc=");
    uart_put_hex(ecall_pc);
    uart_puts(" handler-mepc=");
    uart_put_hex(last_mepc);
    uart_puts(" handler-sepc=");
    uart_put_hex(last_sepc);
    uart_puts(" cause=");
    uart_put_hex(last_cause);
    uart_puts(" count=");
    uart_put_dec(trap_count);
    uart_puts("\n");
    check(trap_count == 1, "trap_count != 1 after deliberate ecall");
    check(last_cause == ECALL_MCAUSE, "trap cause != 11 after ecall");
    check(last_mepc == ecall_pc, "handler mepc != ecall pc");
    check(last_sepc == 0, "sepc clobbered by M-mode trap entry");
    v = read_sepc();
    uart_puts("trap: sepc-readback=");
    uart_put_hex(v);
    uart_puts(" (expect 0x0)\n");
    check(v == 0, "sepc readback != 0 after trap");
    __asm__ volatile("csrr %0, mepc" : "=r"(v));
    uart_puts("trap: mepc-readback=");
    uart_put_hex(v);
    uart_puts(" (expect ecall pc + 4)\n");
    check(v == ecall_pc + 4, "mepc readback != ecall pc + 4");

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
