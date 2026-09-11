// mideleg_main.c: mideleg WARL write/legalized-readback check.
//
// Mechanism under test: mideleg is a WARL (write-any/read-legal)
// CSR. A write is always accepted; the readback is the legalized
// value, the subset of interrupt causes this implementation can
// delegate to the next privilege level. The legalized readback of
// an all-ones write therefore publishes, per run, exactly which
// interrupt bits this hart will let M-mode delegate. A zero write
// on this hart still reads back 0x1444: bits 2, 6, 10, 12 are
// read-only-one (the implementation forces them), so they cannot
// be cleared by software.
//
// Why those values: on QEMU 8.2.2's virt hart the H extension is
// present, and the WARL handler rmw_mideleg64 (target/riscv/csr.c)
// masks every write down to delegable_ints = S_MODE_INTERRUPTS |
// VS_MODE_INTERRUPTS | MIP_LCOFIP (0x2666), then forces on
// HS_MODE_INTERRUPTS = MIP_SGEIP | MIP_VSSIP | MIP_VSTIP |
// MIP_VSEIP (0x1444). An all-ones write therefore reads back
// 0x2666 | 0x1444 = 0x3666, and a zero write reads back the forced
// bits 0x1444. Both values were measured first, then asserted.
//
// Sequence under test:
//   1. Boot: mideleg reads 0x1444 (the forced H bits).
//   2. Install a park-on-entry trap handler (direct-mode mtvec);
//      MIE stays clear and no interrupt source is armed, so no
//      trap should ever fire, and any stray trap is observable
//      as a harness timeout (FAIL).
//   3. csrw all-ones (0xFFFFFFFFFFFFFFFF) to mideleg. The
//      readback must equal EXPECTED_MIDELEG_ONES (0x3666).
//   4. csrw 0 to mideleg. The readback must equal
//      EXPECTED_MIDELEG_ZERO (0x1444): the forced H bits stay set.
//   5. csrw all-ones again. The readback must repeat 0x3666,
//      proving the legalization is stable across writes.
//
// The write/legalized-readback pairs are published in the run log
// and are the evidence: which delegation bits exist on this hart
// is decided by the implementation, and the readback is how it
// declares that.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.

#include "../uart.h"

// Legalized readback of an all-ones write on QEMU 8.2.2 virt: the
// delegable interrupt causes (0x2666) plus the forced H bits
// (0x1444).
#define EXPECTED_MIDELEG_ONES  0x3666UL
// Readback after a zero write: only the forced H bits survive.
#define EXPECTED_MIDELEG_ZERO  0x1444UL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final RESULT line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

static unsigned long read_mideleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mideleg" : "=r"(v));
    return v;
}

static void write_mideleg(unsigned long v) {
    __asm__ volatile("csrw mideleg, %0" : : "r"(v));
}

extern void mideleg_trap_entry(void);

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// Prints one write/legalized-readback pair: the software write
// value and what mideleg reads back after the write. These pairs
// are the evidence for which delegation bits this hart admits.
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
    unsigned long v;
    unsigned long legalized;

    uart_init();
    uart_puts("mideleg-warl: mideleg write/legalized-readback check\n");

    // 1. Boot state: the forced H bits read back before anything
    // runs.
    v = read_mideleg();
    uart_puts("boot: mideleg=");
    uart_put_hex(v);
    uart_puts("\n");
    check(v == EXPECTED_MIDELEG_ZERO, "mideleg != 0x1444 at boot");

    // 2. Defensive handler only: no trap should ever fire in this
    // module, so any trap that does is a FAIL (observable as the
    // harness timeout because the entry never returns).
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mideleg_trap_entry));
    {
        unsigned long tv;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        uart_puts("trap: mtvec=");
        uart_put_hex(tv);
        uart_puts("\n");
        check((tv & ~3UL) == (unsigned long)mideleg_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
    }

    // 3. All-ones write: read back the legalized delegable set.
    write_mideleg(~0UL);
    legalized = read_mideleg();
    print_pair("write-ones", ~0UL, legalized);
    check(legalized == EXPECTED_MIDELEG_ONES,
          "all-ones readback != 0x3666");

    // 4. Zero write: the forced H bits stay set; the readback is
    // the legal value 0x1444, not 0.
    write_mideleg(0UL);
    v = read_mideleg();
    print_pair("write-zero", 0UL, v);
    check(v == EXPECTED_MIDELEG_ZERO,
          "zero write readback != 0x1444");

    // 5. Write all-ones again: legalization must be stable.
    write_mideleg(~0UL);
    v = read_mideleg();
    print_pair("write-ones-again", ~0UL, v);
    check(v == legalized, "second all-ones readback differs");

    uart_puts("RESULT: ");
    uart_puts(fails == 0 ? "PASS" : "FAIL");
    uart_puts(" (checks=6)\n");

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
