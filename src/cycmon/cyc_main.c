// cyc_main.c: rdcycle monotonicity over a fixed 100-nop window
// (backlog item 77).
//
// Mechanism under test: the `cycle` CSR (read by `rdcycle`, i.e.
// `csrr x, cycle`) must never go backward, and the delta across a
// fixed instruction window must be strictly positive. This is the
// cycle-counter ground truth the other benchmark modules stand on,
// so this module measures it directly: 1000 consecutive samples,
// each sample one asm block holding `csrr cycle`, exactly 100 `nop`
// instructions, and `csrr cycle` again, so -O2 cannot insert or
// reorder anything between the two reads.
//
// What the emulator actually does (verified in QEMU 8.2.2 source,
// same as the counter-alias module): CSR_CYCLE reads the host tick
// counter (rdtsc on x86_64) via riscv_pmu_read_ctr. So "cycle"
// units here are host ticks, not guest instructions, and the
// 100-nop delta is the host time QEMU's TCG spends translating and
// executing 100 guest nops plus two emulated CSR reads. Monotonicity
// of the reads is what is under test; the absolute delta values are
// host properties, reported, not gated.
//
// Checks (all computed, none eyeballed):
//   1. Every delta strictly positive (and below 2^40, so a backward
//      counter, which would appear as a huge unsigned value, fails).
//   2. Reads monotonic across samples: the start read of sample i is
//      at or after the end read of sample i-1, i.e.
//      c0[i] - c0[i-1] >= delta[i-1].
//   3. Deltas bounded: at most 1% of samples may exceed 2^20 ticks
//      (~0.9 ms at the observed host tick rate). A stuck or runaway
//      counter would blow this bound on all or most samples; the
//      allowance absorbs rare host scheduling stalls, which do
//      happen on this shared host (one ~1.9 ms stall observed on
//      2026-09-09, caught by the earlier absolute form of this check
//      and re-examined). The outliers are counted and reported, not
//      hidden.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down and
// QEMU exits 0. Measured on this QEMU 8.2.2 build: the 0x3333 fail
// word also shuts down with exit 0, so on FAIL the module instead
// parks the hart in a wfi loop without touching the finisher; the
// bench harness runs QEMU under `timeout`, so a FAIL run is
// observable as the timeout exit status (124) in addition to the
// RESULT: FAIL line.
//
// No trap handler: everything is a plain M-mode CSR read.

#include "../uart.h"

#define NSAMPLES 1000
#define OUTLIER_THRESH (1UL << 20)  // ~0.9 ms at the observed tick rate
#define MAX_OUTLIERS (NSAMPLES / 100)  // 1% host-stall allowance

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

static unsigned long c0[NSAMPLES];    // rdcycle before the nop window
static unsigned long delta[NSAMPLES]; // rdcycle after minus before
static unsigned long sorted[NSAMPLES];

// One sample: rdcycle, exactly 100 nops, rdcycle. One volatile asm
// block; the compiler cannot move code across it.
static void take_sample(unsigned long *before, unsigned long *delta_out) {
    unsigned long b, a;
    __asm__ volatile(
        "csrr %0, cycle\n\t"
        ".rept 100\n\t"
        "nop\n\t"
        ".endr\n\t"
        "csrr %1, cycle"
        : "=r"(b), "=r"(a));
    *before = b;
    *delta_out = a - b;
}

static unsigned long read_mtime(void) {
    return *(volatile unsigned long *)0x0200bff8UL;
}

// Insertion sort on a copy; NSAMPLES is small enough that O(n^2)
// needs no library.
static void sort_copy(void) {
    unsigned long i, j, key;
    for (i = 0; i < NSAMPLES; i++)
        sorted[i] = delta[i];
    for (i = 1; i < NSAMPLES; i++) {
        key = sorted[i];
        j = i;
        while (j > 0 && sorted[j - 1] > key) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = key;
    }
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
    unsigned long i;
    unsigned long min_d, max_d, med_d, outliers;
    unsigned long backward_reads = 0;
    unsigned long t0, t1;

    uart_init();
    uart_puts("cycmon: rdcycle monotonicity over 100-nop window\n");

    t0 = read_mtime();
    for (i = 0; i < NSAMPLES; i++)
        take_sample(&c0[i], &delta[i]);
    t1 = read_mtime();

    sort_copy();
    min_d = sorted[0];
    max_d = sorted[NSAMPLES - 1];
    med_d = sorted[NSAMPLES / 2];
    outliers = 0;
    for (i = 0; i < NSAMPLES; i++)
        if (delta[i] > OUTLIER_THRESH)
            outliers++;

    uart_puts("samples: ");
    uart_put_dec(NSAMPLES);
    uart_puts("\nrun window: ");
    uart_put_dec(t1 - t0);
    uart_puts(" mtime ticks\n");
    uart_puts("delta (after - before): min=");
    uart_put_dec(min_d);
    uart_puts(" median=");
    uart_put_dec(med_d);
    uart_puts(" max=");
    uart_put_dec(max_d);
    uart_puts(" outliers(>2^20)=");
    uart_put_dec(outliers);
    uart_puts("\n");

    // Check 1: every delta strictly positive, and no borrow-sized
    // value (a backward counter would show up above 2^40).
    {
        int ok = 1;
        for (i = 0; i < NSAMPLES; i++)
            if (delta[i] == 0 || delta[i] > (1UL << 40))
                ok = 0;
        check(ok, "non-positive or borrow-sized delta");
    }

    // Check 2: the underlying reads never go backward across the
    // 1000 samples.
    for (i = 1; i < NSAMPLES; i++)
        if (c0[i] - c0[i - 1] < delta[i - 1])
            backward_reads++;
    uart_puts("backward reads across samples: ");
    uart_put_dec(backward_reads);
    uart_puts("\n");
    check(backward_reads == 0, "rdcycle read went backward");

    // Check 3: deltas bounded. A stuck or runaway counter would
    // exceed the threshold on all or most samples; the 1% allowance
    // absorbs rare host scheduling stalls.
    check(outliers <= MAX_OUTLIERS, "more than 1% deltas above 2^20");

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
