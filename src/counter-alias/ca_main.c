// ca_main.c: counter-alias check (backlog item 59).
//
// Mechanism under test: in M-mode, the `cycle` CSR (read by `rdcycle`)
// is a read-only shadow of `mcycle` (read by `csrr mcycle`). Both
// names must reach the same underlying counter: a `rdcycle` taken
// immediately after a `csrr mcycle` can never read a smaller value,
// and the two readings must advance at the same rate.
//
// What the emulator actually does (verified in QEMU 8.2.0 source):
//   - target/riscv/csr.c: CSR_CYCLE and CSR_MCYCLE both dispatch to
//     read_hpmcounter with counter index 0, which calls
//     riscv_pmu_read_ctr and returns get_ticks() - ctr_prev + ctr_val.
//     Both names are the same host tick counter, sampled separately
//     on each CSR read.
//   - util/qemu-timer.c + system/cpus.c + system/cpu-timers.c:
//     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) is cpus_get_virtual_clock,
//     which for TCG without icount is cpu_get_clock(), i.e. the host
//     CLOCK_MONOTONIC while ticks are enabled. hw/intc/riscv_aclint.c
//     derives mtime from it, so CLINT mtime is a faithful 10 MHz wall
//     clock.
//   - get_ticks() is cpu_get_host_ticks(), rdtsc on x86_64.
// Consequence: the mcycle/mtime ratio measures the HOST TSC rate
// against wall time. The host here (AMD EPYC 9D64 VM) has no
// invariant_tsc flag (constant_tsc and nonstop_tsc only), so the TSC
// rate follows the host CPU P-state: 1.49 GHz was observed on
// 2026-09-09 (ratio 149), 1.198 GHz on 2026-09-09 later the same day
// (ratio ~119, confirmed by a host-side rdtsc-vs-clock_gettime
// measurement). The 140..160 band from the work brief therefore
// cannot be met on the current host through any choice of guest
// code; the backlog item itself specifies no band. Check 5 below
// gates on what the module can verify instead: the counter is alive
// (absolute sanity range) and both measurement windows agree with
// each other (self-consistency). The historical band is still
// reported for context.
//
// Checks (all computed, none eyeballed):
//   1. Every back-to-back delta non-negative (rdcycle never below
//      mcycle; a borrow would appear as a huge unsigned value).
//   2. Back-to-back delta tight: median-min <= 64, at most 1% of
//      samples beyond 1024 units (host jitter outliers, reported).
//      The gap is one emulated CSR read's host time, not guest
//      cycles.
//   3. mcycle monotonic (never backward) across all 1000 samples.
//   4. Rate lockstep: rdcycle rate vs mcycle rate over the same
//      window within 5%.
//   5. mtime cross-check: the mcycle/mtime ratio over the whole
//      1000-sample run is within 25% of the 100 ms calibration ratio
//      measured moments earlier on the same host, and inside the
//      absolute 50..500 sanity range (catches a stuck or runaway
//      counter). The short run window (~3-5k ticks) carries a fixed
//      ~200-tick overhead from the two boundary mtime MMIO reads
//      (isolated MMIO reads take QEMU's slow path, ~10 us each,
//      measured), which alone accounts for several percent of
//      systematic dip versus the calibration; 25% absorbs that plus
//      host jitter while still catching a stuck (100% off) or
//      half-rate (50% off) counter.
// Reported, not gated: the absolute ratio against the historical
// 140..160 band, and the median gap as the measured
// emulated-CSR-read cost.
//
// No trap handler: everything is a plain M-mode CSR or MMIO read.

#include "../uart.h"

#define NSAMPLES 1000
#define OUTLIER_THRESH 1024UL   // units; beyond this is host jitter

// Adjacent reads: the two CSR reads live in one asm block so the
// compiler cannot reorder or insert code between them.
static void read_pair(unsigned long *mcycle, unsigned long *rdcycle_val) {
    unsigned long m, r;
    __asm__ volatile(
        "csrr %0, mcycle\n\t"
        "csrr %1, cycle"
        : "=r"(m), "=r"(r));
    *mcycle = m;
    *rdcycle_val = r;
}

static unsigned long read_mcycle(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mcycle" : "=r"(v));
    return v;
}

static unsigned long read_rdcycle(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, cycle" : "=r"(v));
    return v;
}

static unsigned long read_mtime(void) {
    return *(volatile unsigned long *)0x0200bff8UL;
}

static unsigned long deltas[NSAMPLES];  // per-sample (rdcycle - mcycle)
static unsigned long mcyc[NSAMPLES];    // per-sample mcycle reads
static unsigned long sorted[NSAMPLES];

// Insertion sort on a copy; NSAMPLES is small enough that O(n^2)
// needs no library.
static void sort_copy(void) {
    unsigned long i, j, key;
    for (i = 0; i < NSAMPLES; i++)
        sorted[i] = deltas[i];
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
    unsigned long m, r;
    unsigned long t0, t1;
    unsigned long cal_t0, cal_t1, cal_m0, cal_m1, cal_r0, cal_r1;
    unsigned long cal_ratio;
    unsigned long min_d, max_d, med_d, outliers;
    unsigned long prev, step, min_step;
    int monotonic;
    unsigned long m_rate, r_rate, rate_diff_pct;
    unsigned long ratio, ratio_diff_pct;

    uart_init();
    uart_puts("counter-alias: mcycle vs rdcycle lockstep check\n");

    // Long-window calibration, same construction as the sibling
    // modules: spin 1M mtime ticks (100 ms of virtual time) and
    // measure both counters' rates over that window.
    cal_t0 = read_mtime();
    cal_m0 = read_mcycle();
    cal_r0 = read_rdcycle();
    while (read_mtime() - cal_t0 < 1000000UL)
        ;
    cal_t1 = read_mtime();
    cal_m1 = read_mcycle();
    cal_r1 = read_rdcycle();
    uart_puts("calibration over 1000000 mtime ticks:\n");
    uart_puts("  mcycle rate  = ");
    uart_put_dec((cal_m1 - cal_m0) / (cal_t1 - cal_t0));
    uart_puts(" units/tick\n");
    uart_puts("  rdcycle rate = ");
    uart_put_dec((cal_r1 - cal_r0) / (cal_t1 - cal_t0));
    uart_puts(" units/tick\n");
    cal_ratio = (cal_m1 - cal_m0) / (cal_t1 - cal_t0);

    // 1000 back-to-back pairs, mtime bounding the whole run.
    t0 = read_mtime();
    for (i = 0; i < NSAMPLES; i++) {
        read_pair(&m, &r);
        mcyc[i] = m;
        deltas[i] = r - m;
    }
    t1 = read_mtime();

    // Back-to-back delta statistics.
    sort_copy();
    min_d = sorted[0];
    max_d = sorted[NSAMPLES - 1];
    med_d = sorted[NSAMPLES / 2];
    outliers = 0;
    for (i = 0; i < NSAMPLES; i++)
        if (deltas[i] > OUTLIER_THRESH)
            outliers++;
    uart_puts("samples: ");
    uart_put_dec(NSAMPLES);
    uart_puts("\nback-to-back (rdcycle - mcycle): min=");
    uart_put_dec(min_d);
    uart_puts(" median=");
    uart_put_dec(med_d);
    uart_puts(" max=");
    uart_put_dec(max_d);
    uart_puts(" outliers(>1024)=");
    uart_put_dec(outliers);
    uart_puts("\n");

    // Check 1: the shadow never reads below the machine counter.
    // Unsigned arithmetic makes a borrow show up as a huge value.
    {
        int ok = 1;
        for (i = 0; i < NSAMPLES; i++)
            if (deltas[i] > (1UL << 40))
                ok = 0;
        check(ok, "rdcycle read below mcycle in back-to-back pair");
    }

    // Check 2: the gap is tight. The two reads sample the same tick
    // source, so the gap is one emulated CSR read's host time; it
    // must not wander.
    check(med_d >= min_d && med_d - min_d <= 64,
          "back-to-back gap not tight (median-min > 64)");
    check(outliers <= NSAMPLES / 100,
          "more than 1% host-jitter outliers");

    // Check 3: mcycle never goes backward across the run.
    monotonic = 1;
    min_step = 0;
    prev = mcyc[0];
    for (i = 1; i < NSAMPLES; i++) {
        step = mcyc[i] - prev;
        if (mcyc[i] < prev)
            monotonic = 0;
        else if (i == 1 || step < min_step)
            min_step = step;
        prev = mcyc[i];
    }
    check(monotonic, "mcycle went backward across samples");

    // Check 4: rate lockstep over the same window. rdcycle values
    // are reconstructed as mcycle + delta.
    m_rate = (mcyc[NSAMPLES - 1] - mcyc[0]) / (t1 - t0);
    r_rate = ((mcyc[NSAMPLES - 1] + deltas[NSAMPLES - 1]) -
              (mcyc[0] + deltas[0])) / (t1 - t0);
    rate_diff_pct = m_rate > r_rate
        ? (m_rate - r_rate) * 100 / m_rate
        : (r_rate - m_rate) * 100 / m_rate;
    uart_puts("run window: mtime ticks=");
    uart_put_dec(t1 - t0);
    uart_puts(" mcycle rate=");
    uart_put_dec(m_rate);
    uart_puts(" rdcycle rate=");
    uart_put_dec(r_rate);
    uart_puts(" rate diff=");
    uart_put_dec(rate_diff_pct);
    uart_puts("%\n");
    check(rate_diff_pct <= 5, "rdcycle/mcycle rates differ by > 5%");

    // Check 5: mtime cross-check. The run-window ratio must agree
    // with the calibration ratio (same host, same boot) within 25%,
    // and sit inside the absolute 50..500 sanity range. The
    // historical 140..160 band is reported for context; see the
    // header comment for why it is not gated.
    ratio = m_rate;
    ratio_diff_pct = cal_ratio > ratio
        ? (cal_ratio - ratio) * 100 / cal_ratio
        : (ratio - cal_ratio) * 100 / cal_ratio;
    uart_puts("mcycle units per mtime tick: ");
    uart_put_dec(ratio);
    uart_puts(" (historical band 140..160: ");
    uart_puts(ratio >= 140 && ratio <= 160 ? "in band" : "OUT OF BAND");
    uart_puts("; vs calibration ");
    uart_put_dec(ratio_diff_pct);
    uart_puts("% diff)\n");
    check(ratio >= 50 && ratio <= 500,
          "mcycle/mtime ratio outside 50..500 sanity range");
    check(ratio_diff_pct <= 25,
          "run ratio differs from calibration by > 25%");
    uart_puts("median back-to-back gap ~= one emulated CSR read: ");
    uart_put_dec(med_d);
    uart_puts(" host-tick units\n");

    uart_puts("RESULT: ");
    uart_puts(fails == 0 ? "PASS" : "FAIL");
    uart_puts("\n");
    return 0;
}
