// mtw_main.c: CLINT mtime write/readback/advance measurement
// (backlog item 135).
//
// Mechanism under test: whether a store to the memory-mapped mtime
// register takes effect on this machine, and whether the counter
// keeps advancing afterward. The module:
//
//   1. Reads mtime twice and requires the second read to be larger
//      (baseline advancement).
//   2. Writes the constant 0x100000000 to mtime as two 32-bit
//      stores (low word, then high word).
//   3. Reads back immediately with a stable-pair 32-bit read and
//      publishes the write/readback/delta triple. The readback must
//      be >= the written value and within a small delta
//      (MAX_IMMEDIATE_DELTA mtime ticks), so the report
//      distinguishes "write honored" from "write ignored".
//   4. Takes four further readbacks and requires every one to be
//      strictly larger than the previous sample, all at or above
//      the written base.
//
// Access size: 64-bit accesses to CLINT registers fault on this
// emulator (observed with the msip register in src/msip/, which
// uses the 32-bit form), so mtime is written and read with 32-bit
// accesses only; no 64-bit CLINT access is ever issued. A torn read
// (the counter crossing a 32-bit boundary between the low and high
// loads) would corrupt the 64-bit value, so the read sequence is
// high, low, high, keeping the pair only when the two high reads
// agree. The written value 0x100000000 (2^32 ticks, about 429
// seconds of virtual time at the 10 MHz timebase) is unreachable by
// natural advancement within the few seconds of wall time the
// machine has been up, so readback >= written genuinely
// discriminates a stuck write from an ignored one.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop
// without touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.
//
// No trap handler: everything is a plain M-mode load/store to the
// memory-mapped CLINT register, with interrupts left disabled.

#include "../uart.h"

#define CLINT_MTIME      0x0200bff8UL  // 64-bit mtime, 10 MHz timebase
#define CLINT_MTIME_LO ((volatile unsigned int *)0x0200bff8UL)
#define CLINT_MTIME_HI ((volatile unsigned int *)(0x0200bff8UL + 4))

#define WRITE_VAL  0x100000000UL  // the known constant written to mtime
#define MAX_IMMEDIATE_DELTA 100000UL  // readback must be this close to WRITE_VAL
#define NSAMPLES   4

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// Stable-pair read with 32-bit loads: read the high word, the low
// word, then the high word again, and keep the low/high pair only
// when the two high reads agree. The volatile qualifiers force the
// compiler to emit each load exactly once, in order; no load is
// merged or reordered.
static unsigned long read_mtime(void) {
    unsigned long h1, h2, l;
    do {
        h1 = *CLINT_MTIME_HI;
        l = *CLINT_MTIME_LO;
        h2 = *CLINT_MTIME_HI;
    } while (h1 != h2);
    return (h1 << 32) | l;
}

// Write mtime as two 32-bit stores: low word first, then the high
// word. The register holds the written value only after both stores
// complete; nothing else observes mtime in between (single hart,
// interrupts disabled).
static void write_mtime(unsigned long v) {
    *CLINT_MTIME_LO = (unsigned int)(v & 0xffffffffUL);
    *CLINT_MTIME_HI = (unsigned int)(v >> 32);
}

static unsigned long rdcycle(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, cycle" : "=r"(v));
    return v;
}

// rdcycle on this QEMU follows the host clock, not the 10 MHz mtime
// timebase. Spin for 1M mtime ticks (100 ms of virtual time) and
// report rdcycle units per tick so the timing numbers are
// interpretable as host time. (Same construction as src/msip/.)
static unsigned long calibration(void) {
    unsigned long t0, t1, c0, c1, ratio;

    t0 = read_mtime();
    c0 = rdcycle();
    while (read_mtime() - t0 < 1000000UL)
        ;
    t1 = read_mtime();
    c1 = rdcycle();
    ratio = (c1 - c0) / (t1 - t0);
    uart_puts("clock: 1000000 mtime ticks -> rdcycle delta ");
    uart_put_dec(c1 - c0);
    uart_puts(" (ratio ");
    uart_put_dec(ratio);
    uart_puts(" rdcycle units per tick)\n");
    return ratio;
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
    unsigned long t0, t1, r;
    unsigned long samples[NSAMPLES];
    unsigned long delta;
    unsigned long write_stuck;  // readback >= written value and small delta
    unsigned long i, strictly_advancing = 1, above_base = 1;

    uart_init();
    uart_puts("mtime-write: CLINT mtime write/readback/advance measurement\n");

    (void)calibration();

    // 1. Baseline: two reads; the second must be strictly larger.
    t0 = read_mtime();
    t1 = read_mtime();
    uart_puts("baseline: mtime0=");
    uart_put_hex(t0);
    uart_puts(" mtime1=");
    uart_put_hex(t1);
    uart_puts(" delta=");
    uart_put_dec(t1 - t0);
    uart_puts("\n");
    check(t1 > t0, "baseline mtime did not advance between two reads");

    // 2+3. Write the known constant, read back immediately, publish
    // the triple. Unsigned subtraction r - WRITE_VAL is the delta
    // when r >= WRITE_VAL; it wraps to a huge value when the write
    // was ignored and the readback is below the written value, which
    // fails the MAX_IMMEDIATE_DELTA bound below and prints the
    // actual values.
    write_mtime(WRITE_VAL);
    r = read_mtime();
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
        samples[i] = read_mtime();
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
