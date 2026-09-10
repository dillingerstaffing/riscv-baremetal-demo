// crl_main.c: rdcycle read-latency floor (backlog item 145).
//
// Mechanism under test: the `cycle` CSR, read by `rdcycle`
// (`csrr x, cycle`). On QEMU 8.2.2 this samples the host tick
// counter (rdtsc on x86_64), so the delta between two back-to-back
// reads is the host time for one emulated CSR read: the
// read-latency floor of the counter itself. The module reads
// `cycle` back to back with zero instructions between reads (bursts
// of 27, one read per register, a single volatile asm block per
// burst) and publishes the min/median/max delta between successive
// reads, a 16-bin histogram, and an rdcycle-vs-mtime calibration so
// the deltas are interpretable in mtime ticks.
//
// Reads per run: 38 bursts of 27 back-to-back reads = 1026 reads.
// Deltas are taken only over the 26 consecutive pairs inside each
// burst (988 deltas); the 37 boundary intervals contain 27 stores
// and the loop bookkeeping, so they are excluded by construction,
// not hidden. The shipped binary's disassembly is checked for the
// 27-instruction back-to-back read burst (see PROOF.md).

#include "../uart.h"

#define NBURSTS 38
#define REGS_PER_BURST 27
#define NREADS (NBURSTS * REGS_PER_BURST)        // 1026
#define NDELTAS (NBURSTS * (REGS_PER_BURST - 1)) // 988
#define NBINS 16
#define DELTA_CAP (1UL << 40) // above this a delta is a backward read

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u
#define MTIME ((volatile unsigned long *)0x0200bff8UL)

static unsigned long reads[NREADS];
static unsigned long delta[NDELTAS];
static unsigned long sorted[NDELTAS];
static unsigned long hist[NBINS];

// Trap record area, written only by crl_trap.S. No trap is expected;
// if one fires, the handler records mcause/mepc/mtval and parks the
// hart, so a trap is observable as a missing RESULT line plus the
// harness timeout exit status.
volatile unsigned long crl_trap_rec[4];
volatile unsigned long crl_trap_count;

extern void crl_trap_entry(void);

static unsigned long read_mtime(void) {
    return *MTIME;
}

// One burst: 27 consecutive `csrr x, cycle` with zero instructions
// between them, each into a distinct register. One volatile asm
// block, so the compiler emits the reads verbatim back to back.
// s0-s11 are callee-saved, but the compiler's save/restore wraps
// the block and the outputs are copied to dst before any restore,
// so this is safe. x1 (ra) is deliberately not used.
static void take_burst(unsigned long *dst) {
    register unsigned long v05 __asm__("x5");
    register unsigned long v06 __asm__("x6");
    register unsigned long v07 __asm__("x7");
    register unsigned long v28 __asm__("x28");
    register unsigned long v29 __asm__("x29");
    register unsigned long v30 __asm__("x30");
    register unsigned long v31 __asm__("x31");
    register unsigned long v10 __asm__("x10");
    register unsigned long v11 __asm__("x11");
    register unsigned long v12 __asm__("x12");
    register unsigned long v13 __asm__("x13");
    register unsigned long v14 __asm__("x14");
    register unsigned long v15 __asm__("x15");
    register unsigned long v16 __asm__("x16");
    register unsigned long v17 __asm__("x17");
    register unsigned long v08 __asm__("x8");
    register unsigned long v09 __asm__("x9");
    register unsigned long v18 __asm__("x18");
    register unsigned long v19 __asm__("x19");
    register unsigned long v20 __asm__("x20");
    register unsigned long v21 __asm__("x21");
    register unsigned long v22 __asm__("x22");
    register unsigned long v23 __asm__("x23");
    register unsigned long v24 __asm__("x24");
    register unsigned long v25 __asm__("x25");
    register unsigned long v26 __asm__("x26");
    register unsigned long v27 __asm__("x27");
    __asm__ volatile(
        "csrr x5, cycle\n\t"
        "csrr x6, cycle\n\t"
        "csrr x7, cycle\n\t"
        "csrr x28, cycle\n\t"
        "csrr x29, cycle\n\t"
        "csrr x30, cycle\n\t"
        "csrr x31, cycle\n\t"
        "csrr x10, cycle\n\t"
        "csrr x11, cycle\n\t"
        "csrr x12, cycle\n\t"
        "csrr x13, cycle\n\t"
        "csrr x14, cycle\n\t"
        "csrr x15, cycle\n\t"
        "csrr x16, cycle\n\t"
        "csrr x17, cycle\n\t"
        "csrr x8, cycle\n\t"
        "csrr x9, cycle\n\t"
        "csrr x18, cycle\n\t"
        "csrr x19, cycle\n\t"
        "csrr x20, cycle\n\t"
        "csrr x21, cycle\n\t"
        "csrr x22, cycle\n\t"
        "csrr x23, cycle\n\t"
        "csrr x24, cycle\n\t"
        "csrr x25, cycle\n\t"
        "csrr x26, cycle\n\t"
        "csrr x27, cycle"
        : "=r"(v05), "=r"(v06), "=r"(v07), "=r"(v28), "=r"(v29),
          "=r"(v30), "=r"(v31), "=r"(v10), "=r"(v11), "=r"(v12),
          "=r"(v13), "=r"(v14), "=r"(v15), "=r"(v16), "=r"(v17),
          "=r"(v08), "=r"(v09), "=r"(v18), "=r"(v19), "=r"(v20),
          "=r"(v21), "=r"(v22), "=r"(v23), "=r"(v24), "=r"(v25),
          "=r"(v26), "=r"(v27));
    dst[0] = v05;
    dst[1] = v06;
    dst[2] = v07;
    dst[3] = v28;
    dst[4] = v29;
    dst[5] = v30;
    dst[6] = v31;
    dst[7] = v10;
    dst[8] = v11;
    dst[9] = v12;
    dst[10] = v13;
    dst[11] = v14;
    dst[12] = v15;
    dst[13] = v16;
    dst[14] = v17;
    dst[15] = v08;
    dst[16] = v09;
    dst[17] = v18;
    dst[18] = v19;
    dst[19] = v20;
    dst[20] = v21;
    dst[21] = v22;
    dst[22] = v23;
    dst[23] = v24;
    dst[24] = v25;
    dst[25] = v26;
    dst[26] = v27;
}

// rdcycle-vs-mtime calibration: spin until mtime advances 10000
// ticks and report the cycle delta over the mtime delta. The skew
// from reading mtime between the two cycle reads is a handful of
// ticks against a 10000-tick window.
static void calibrate(unsigned long *dcy, unsigned long *dmt) {
    unsigned long c0, c1, m0, m1;
    __asm__ volatile("csrr %0, cycle" : "=r"(c0));
    m0 = read_mtime();
    do {
        m1 = read_mtime();
    } while (m1 - m0 < 10000UL);
    __asm__ volatile("csrr %0, cycle" : "=r"(c1));
    *dcy = c1 - c0;
    *dmt = m1 - m0;
}

// Insertion sort on a copy; NDELTAS is small enough that O(n^2)
// needs no library.
static void sort_copy(void) {
    unsigned long i, j, key;
    for (i = 0; i < NDELTAS; i++)
        sorted[i] = delta[i];
    for (i = 1; i < NDELTAS; i++) {
        key = sorted[i];
        j = i;
        while (j > 0 && sorted[j - 1] > key) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = key;
    }
}

int main(void) {
    unsigned long b, i, k;
    unsigned long dcy, dmt;
    unsigned long min_d, max_d, med_d, width, bin;
    unsigned long bad_deltas = 0;

    uart_init();
    uart_puts("cycle-read-latency: rdcycle read-latency floor\n");

    __asm__ volatile("csrw mtvec, %0" ::"r"((unsigned long)crl_trap_entry));
    __asm__ volatile("csrw mscratch, %0" ::"r"((unsigned long)crl_trap_rec));

    calibrate(&dcy, &dmt);
    uart_puts("calibration: ");
    uart_put_dec(dcy);
    uart_puts(" cycles over ");
    uart_put_dec(dmt);
    uart_puts(" mtime ticks = ");
    uart_put_dec(dcy / dmt);
    uart_puts(" + ");
    uart_put_dec(dcy % dmt);
    uart_puts("/");
    uart_put_dec(dmt);
    uart_puts(" cycles per mtime tick\n");

    for (b = 0; b < NBURSTS; b++)
        take_burst(&reads[b * REGS_PER_BURST]);

    // Deltas only over consecutive pairs inside a burst.
    k = 0;
    for (b = 0; b < NBURSTS; b++)
        for (i = 0; i < REGS_PER_BURST - 1; i++)
            delta[k++] = reads[b * REGS_PER_BURST + i + 1] -
                         reads[b * REGS_PER_BURST + i];

    sort_copy();
    min_d = sorted[0];
    max_d = sorted[NDELTAS - 1];
    med_d = sorted[NDELTAS / 2];

    width = (max_d + NBINS) / NBINS; // ceiling of (max+1)/16
    for (i = 0; i < NBINS; i++)
        hist[i] = 0;
    for (i = 0; i < NDELTAS; i++) {
        bin = delta[i] / width;
        if (bin >= NBINS)
            bin = NBINS - 1;
        hist[bin]++;
    }

    for (i = 0; i < NDELTAS; i++)
        if (delta[i] == 0 || delta[i] > DELTA_CAP)
            bad_deltas++;

    uart_puts("reads: ");
    uart_put_dec(NREADS);
    uart_puts(" in ");
    uart_put_dec(NBURSTS);
    uart_puts(" bursts of ");
    uart_put_dec(REGS_PER_BURST);
    uart_puts("; deltas: ");
    uart_put_dec(NDELTAS);
    uart_puts("\n");
    uart_puts("delta between successive reads: min=");
    uart_put_dec(min_d);
    uart_puts(" median=");
    uart_put_dec(med_d);
    uart_puts(" max=");
    uart_put_dec(max_d);
    uart_puts(" (host ticks)\n");
    uart_puts("histogram, 16 bins, width=");
    uart_put_dec(width);
    uart_puts(" ticks:\n");
    for (i = 0; i < NBINS; i++) {
        uart_puts("  bin ");
        uart_put_dec(i);
        uart_puts(" [");
        uart_put_dec(i * width);
        uart_puts("..");
        uart_put_dec((i + 1) * width - 1);
        uart_puts("]: ");
        uart_put_dec(hist[i]);
        uart_puts("\n");
    }
    uart_puts("bad deltas (0 or >2^40): ");
    uart_put_dec(bad_deltas);
    uart_puts("\ntraps: ");
    uart_put_dec(crl_trap_count);
    uart_puts("\n");

    uart_puts("RESULT: ");
    uart_puts(bad_deltas == 0 && crl_trap_count == 0 ? "PASS" : "FAIL");
    uart_puts("\n");

    // Let the UART drain before touching the finisher device.
    {
        unsigned long drain = read_mtime();
        while (read_mtime() - drain < 100000UL)
            ;
    }

    if (bad_deltas == 0 && crl_trap_count == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS; // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    // FAIL: park the hart without touching the finisher device. The
    // harness runs QEMU under timeout, so a FAIL is observable as
    // the timeout exit status (124) as well as the RESULT line.
    for (;;)
        __asm__ volatile("wfi");
}
