// mcw_main.c: mcycle write/readback/advance measurement
// (backlog item 110).
//
// Mechanism under test: whether a CSR write to mcycle takes effect
// on this machine, and whether the counter advances strictly after
// the write. The module:
//
//   1. Reads mcycle twice and requires the second read to be larger
//      (baseline advancement).
//   2. Writes the constant 0x100000000 to mcycle with csrw.
//   3. Reads back immediately and publishes the write/readback/delta
//      triple. The readback must be >= the written value and within
//      a small delta (MAX_IMMEDIATE_DELTA host-tick units), so the
//      report distinguishes "write honored" from "write ignored".
//   4. Takes four further readbacks and requires every one to be
//      strictly larger than the previous sample, all at or above
//      the written base.
//
// mcycle is WARL: the specification only requires a write followed
// by a read to return the written value filtered through the
// implementation's behavior. This module reports ground truth
// whatever it is: if the write does not stick, the FAIL lines print
// the actual readback values and the verdict says so. The module
// checks only what it can verify on this machine and reports
// everything else as data.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop
// without touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.
//
// No trap handler: everything is a plain M-mode CSR read/write.

#include "../uart.h"

#define WRITE_VAL  0x100000000UL  // the known constant written to mcycle
#define MAX_IMMEDIATE_DELTA 100000UL  // readback must be this close to WRITE_VAL
#define NSAMPLES   4

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

static unsigned long read_mcycle(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mcycle" : "=r"(v));
    return v;
}

static void write_mcycle(unsigned long v) {
    __asm__ volatile("csrw mcycle, %0" :: "r"(v));
}

static unsigned long read_mtime(void) {
    return *(volatile unsigned long *)0x0200bff8UL;
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
    unsigned long c0, c1, r;
    unsigned long samples[NSAMPLES];
    unsigned long delta;
    unsigned long write_stuck;  // readback >= written value and small delta
    unsigned long i, strictly_advancing = 1, above_base = 1;

    uart_init();
    uart_puts("mcycle-write: mcycle write/readback/advance measurement\n");

    // 1. Baseline: two reads; the second must be strictly larger.
    c0 = read_mcycle();
    c1 = read_mcycle();
    uart_puts("baseline: mcycle0=");
    uart_put_hex(c0);
    uart_puts(" mcycle1=");
    uart_put_hex(c1);
    uart_puts(" delta=");
    uart_put_dec(c1 - c0);
    uart_puts("\n");
    check(c1 > c0, "baseline mcycle did not advance between two reads");

    // 2+3. Write the known constant, read back immediately, publish
    // the triple. Unsigned subtraction r - WRITE_VAL is the delta
    // when r >= WRITE_VAL; it wraps to a huge value when the write
    // was ignored and the readback is below the written value, which
    // fails the MAX_IMMEDIATE_DELTA bound below and prints the
    // actual values.
    write_mcycle(WRITE_VAL);
    r = read_mcycle();
    delta = r - WRITE_VAL;
    uart_puts("write/readback: written=");
    uart_put_hex(WRITE_VAL);
    uart_puts(" readback=");
    uart_put_hex(r);
    uart_puts(" delta=");
    uart_put_dec(delta);
    uart_puts("\n");

    write_stuck = (delta <= MAX_IMMEDIATE_DELTA);
    uart_puts("write took effect (readback >= written, delta <= ");
    uart_put_dec(MAX_IMMEDIATE_DELTA);
    uart_puts("): ");
    uart_puts(write_stuck ? "yes" : "no");
    uart_puts("\n");

    check(r >= WRITE_VAL, "readback is below the written value: write ignored");
    check(delta <= MAX_IMMEDIATE_DELTA,
          "readback more than MAX_IMMEDIATE_DELTA above written value");

    // 4. Four further readbacks; each must be strictly larger than
    // the previous sample and no sample may fall below the written
    // base (the counter advances monotonically from the written
    // value onward).
    uart_puts("advance:");
    for (i = 0; i < NSAMPLES; i++) {
        unsigned long prev = (i == 0) ? r : samples[i - 1];
        samples[i] = read_mcycle();
        uart_puts(" ");
        uart_put_hex(samples[i]);
        if (samples[i] <= prev)
            strictly_advancing = 0;
        if (samples[i] < WRITE_VAL)
            above_base = 0;
    }
    uart_puts("\n");
    uart_puts("advance samples strictly increasing: ");
    uart_puts(strictly_advancing ? "yes" : "no");
    uart_puts("\n");
    uart_puts("advance samples all >= written base: ");
    uart_puts(above_base ? "yes" : "no");
    uart_puts("\n");

    check(strictly_advancing, "advance samples not strictly increasing");
    check(above_base, "an advance sample fell below the written base");

    uart_puts("RESULT: ");
    uart_puts(fails == 0 ? "PASS" : "FAIL");
    uart_puts("\n");

    // Let the UART drain before touching the finisher device.
    {
        unsigned long drain = read_mtime();
        while (read_mtime() - drain < 100000UL)
            ;
    }

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
