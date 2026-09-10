// lrsc_main.c: aligned LR/SC attempt-histogram experiment.
//
// Single hart, M-mode, no contention. Runs 10,000 iterations of an
// lr.w/sc.w pair on one aligned word. Each iteration retries the pair
// until sc.w reports success (rd == 0) and counts how many attempts
// that took; the program prints the attempts-to-success histogram.
// The value stored in each iteration is the iteration index, read back
// after the successful sc and checked, so every store is verified by
// round-trip. A minimal M-mode trap handler (lrsc_trap.S) records
// mcause/mepc/mtval into lrsc_save and halts the hart; no trap is
// expected in this uncontended aligned case, and the program checks
// the handler's seen flag as part of the verdict, so an unexpected
// trap would fail the run instead of passing silently.
//
// The retry loop is a single asm block: lr.w immediately followed by
// sc.w with no instruction in between, which is exactly the shape the
// reservation mechanism is defined over.

#include "../uart.h"

extern void lrsc_trap_entry(void);

#define NITERS 10000
#define NBINS 64

// Trap save area for lrsc_trap.S:
// [0]=mcause [1]=mepc [2]=mtval [3]=seen flag
volatile unsigned long lrsc_save[4];

static volatile unsigned int cell __attribute__((aligned(4)));
static unsigned long hist[NBINS];
static unsigned long overflow;

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long csr_read_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

// One iteration: retry the lr.w/sc.w pair until sc reports success.
// Returns the number of attempts. Stores val into cell.
static unsigned long lrsc_store(unsigned int val) {
    unsigned long sc, attempts = 0;
    do {
        __asm__ volatile(
            "lr.w t1, 0(%1)\n\t"
            "sc.w %0, %2, 0(%1)\n\t"
            : "=&r" (sc)
            : "r" (&cell), "r" (val)
            : "t1", "memory");
        attempts++;
    } while (sc != 0);
    return attempts;
}

int main(void) {
    unsigned long i, att, sum, max_att, value_errors;
    unsigned int rb;

    uart_init();
    uart_puts("lrsc-histogram: 10000 aligned lr.w/sc.w pairs, single hart\n");
    uart_puts("hart mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    // Install the trap vector (direct mode) and arm mscratch. No trap
    // is expected; the handler halts the hart, so a trap would end the
    // run without printing RESULT: PASS.
    __asm__ volatile("csrw mtvec, %0" :: "r"(lrsc_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(lrsc_save));
    uart_puts("trap vector installed at ");
    uart_put_hex((unsigned long)lrsc_trap_entry);
    uart_puts(" (halts the hart on any trap)\n");

    cell = 0xDEADBEEFU;

    value_errors = 0;
    max_att = 0;
    for (i = 0; i < NITERS; i++) {
        att = lrsc_store((unsigned int)i);
        if (att <= NBINS)
            hist[att - 1]++;
        else
            overflow++;
        if (att > max_att)
            max_att = att;
        rb = cell;
        if (rb != (unsigned int)i)
            value_errors++;
    }

    uart_puts("attempts-to-success histogram:\n");
    sum = 0;
    for (i = 0; i < NBINS; i++) {
        if (hist[i] != 0) {
            uart_puts("  attempts=");
            uart_put_dec(i + 1);
            uart_puts(" count=");
            uart_put_dec(hist[i]);
            uart_puts("\n");
        }
        sum += hist[i];
    }
    if (overflow != 0) {
        uart_puts("  attempts>64 count=");
        uart_put_dec(overflow);
        uart_puts("\n");
    }
    sum += overflow;

    uart_puts("iterations=");
    uart_put_dec(NITERS);
    uart_puts(" histogram-sum=");
    uart_put_dec(sum);
    uart_puts(" max-attempts=");
    uart_put_dec(max_att);
    uart_puts(" value-errors=");
    uart_put_dec(value_errors);
    uart_puts("\n");

    check(sum == NITERS, "histogram does not account for all iterations");
    check(value_errors == 0, "stored value readback mismatch");
    check(lrsc_save[3] == 0, "unexpected trap fired during the run");
    if (lrsc_save[3] != 0) {
        uart_puts("  trap mcause=");
        uart_put_hex(lrsc_save[0]);
        uart_puts(" mepc=");
        uart_put_hex(lrsc_save[1]);
        uart_puts(" mtval=");
        uart_put_hex(lrsc_save[2]);
        uart_puts("\n");
    }

    if (fails == 0)
        uart_puts("RESULT: PASS\n");
    else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
