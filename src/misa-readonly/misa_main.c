// misa_main.c: misa WARL read-only check (backlog item 142).
//
// Mechanism under test: whether any bit of the misa CSR is writable
// on this hart. The program:
//
//   1. Reads misa at boot as the baseline value.
//   2. Writes all-ones (0xFFFFFFFFFFFFFFFF) to misa with csrw.
//   3. Reads misa back. The readback must be bit-identical to the
//      boot value: the write was ignored, so no bit of misa is
//      writable on this hart.
//   4. Continues normal execution afterward (UART output plus a
//      small deterministic computation) to show the hart is
//      unaffected by the write.
//
// A trap handler is installed (direct-mode mtvec, mscratch scratch
// area) but must never fire: csrw misa is a legal M-mode CSR write,
// so any trap, synchronous or asynchronous, means the write (or the
// run) disturbed the hart and fails the verdict.
//
// A failed check prints FAIL and flips the verdict; RESULT: PASS is
// printed only when every check held.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.

#include "../uart.h"

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

#define MISA_ALL_ONES 0xFFFFFFFFFFFFFFFFUL

extern void misa_trap_entry(void);

// Trap scratch: recorded mcause, mepc, mtval, and trap count.
// BSS-cleared to zero by boot.S.
unsigned long misa_regs[4];

static unsigned long read_misa(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, misa" : "=r"(v));
    return v;
}

static void write_misa(unsigned long v) {
    __asm__ volatile("csrw misa, %0" : : "r"(v));
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

int main(void) {
    unsigned long mtvec;
    unsigned long boot, readback;
    unsigned long acc, i;

    uart_init();
    uart_puts("misa-readonly: misa WARL read-only check\n");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the recording area.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)misa_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)misa_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    check((mtvec & ~3UL) == (unsigned long)misa_trap_entry,
          "mtvec did not take the handler address");
    check((mtvec & 3UL) == 0, "mtvec not in direct mode");

    // 1. Baseline: read misa at boot. This value is reported, never
    // assumed; the verdict below is readback == boot, whatever the
    // value is.
    boot = read_misa();
    uart_puts("boot: misa=");
    uart_put_hex(boot);
    uart_puts("\n");

    // 2. Write all-ones to misa.
    write_misa(MISA_ALL_ONES);

    // 3. Read back. If the hart treated misa as read-only, the write
    // was ignored and the readback equals the boot value exactly.
    readback = read_misa();
    uart_puts("write: value=0xffffffffffffffff readback=");
    uart_put_hex(readback);
    uart_puts("\n");
    check(readback == boot,
          "misa readback differs from boot value: some bits are writable");

    // 4. The hart keeps running normally: UART output above already
    // proves the console path, and this checksum proves the ALU
    // path after the misa write.
    acc = 0;
    for (i = 1; i <= 100; i++)
        acc += i;
    uart_puts("alive: sum1to100=");
    uart_put_dec(acc);
    uart_puts("\n");
    check(acc == 5050, "arithmetic after misa write gave the wrong result");

    // The trap handler must never have fired.
    if (misa_regs[3] != 0) {
        uart_puts("  FAIL: unexpected trap(s), count=");
        uart_put_dec(misa_regs[3]);
        uart_puts(" mcause=");
        uart_put_hex(misa_regs[0]);
        uart_puts(" mepc=");
        uart_put_hex(misa_regs[1]);
        uart_puts(" mtval=");
        uart_put_hex(misa_regs[2]);
        uart_puts("\n");
        fails++;
    }
    uart_puts("traps: count=");
    uart_put_dec(misa_regs[3]);
    uart_puts("\n");

    if (fails == 0) {
        uart_puts("RESULT: PASS\n");
        *VIRT_TEST_FINISHER = FINISHER_PASS;
        for (;;) { }
    }
    uart_puts("RESULT: FAIL\n");
    // Park the hart; the harness observes the timeout exit status.
    for (;;) {
        __asm__ volatile("wfi");
    }
}
