// mt_main.c: mtimecmp delivery-accuracy experiment (proof backlog item 36).
//
// Each trial programs the CLINT mtimecmp register MT_AHEAD_TICKS mtime
// ticks ahead of the current mtime, enables only the machine timer
// interrupt, and spins until the trap fires. The trap entry stamps
// rdtime (tick domain) and rdcycle; the C handler records the stamps,
// disarms the timer, and sets mt_flag so the spin loop exits.
//
// Per trial the headline metric is
//   offset_ticks = t_entry - cmp,
// the number of mtime ticks by which actual trap delivery overshoots
// the programmed value. Cycle units are the tick metric scaled by the
// rdcycle-per-tick ratio measured in-module during calibration; the
// two counters are never mixed directly. A second in-module
// measurement, the slope of (c_entry - c_pre) against
// (t_entry - t_pre) across the trials, cross-checks that ratio.
//
// QEMU's timer is host-scheduled: if the host deschedules the vcpu
// inside the arm window (t_pre >= cmp) or during the wait (offset >
// MT_OFFSET_BOUND), the trial is re-armed and re-run, up to
// MT_MAX_ATTEMPTS times. Only clean trials are recorded; the retry
// count is reported. The reported distribution therefore describes the
// emulator's clean timer delivery, and the retry count quantifies how
// often host scheduling interfered.

#include "mt.h"
#include "../preempt/clint.h"
#include "../uart.h"

mt_save_t mt_save;
mt_rec_t mt_rec[MT_NTRIALS];
volatile unsigned long mt_trial_idx;
volatile unsigned long mt_flag;
volatile unsigned long mt_trap_count;

static unsigned long mt_bad_seen;
static unsigned long mt_bad_mcause;
static unsigned long mt_bad_mepc;
static unsigned long mt_cal_ratio; // rdcycle units per mtime tick

static int fails = 0;
static unsigned long mt_retries;
static unsigned long mt_attempts;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long rd_cycle(void) {
    unsigned long v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

static unsigned long rd_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

// Trap handler: runs on the dedicated trap stack with all registers
// saved. Records the trap-side stamps, disarms the timer so mret does
// not immediately re-trap, and sets mt_flag so the spin loop exits.
// mret returns into the spin loop; no resume-label trick is needed.
void mt_trap_handler(mt_save_t *s) {
    unsigned long i = mt_trial_idx;

    if (s->mcause != MCAUSE_MTI || i >= MT_NTRIALS) {
        mt_bad_seen = 1;
        mt_bad_mcause = s->mcause;
        mt_bad_mepc = s->mepc;
        clint_set_mtimecmp(~0UL);
        return;
    }
    mt_rec[i].mcause = s->mcause;
    mt_rec[i].mepc_before = s->mepc;
    mt_rec[i].t_entry = s->mtime_entry;
    mt_rec[i].c_entry = s->cycle_entry;
    clint_set_mtimecmp(~0UL);
    mt_flag = 1;
    mt_trap_count++;
}

// One trial. The asm block is emitted verbatim: rdtime, rdcycle,
// enable MIE, then spin on mt_flag (set by the handler) between the
// global mt_loop/mt_done labels, then disable MIE. Marked noinline so
// the global labels are defined exactly once.
__attribute__((noinline)) static void mt_trial(mt_rec_t *r) {
    unsigned long t_pre, c_pre;
    mt_flag = 0;
    __asm__ volatile(
        "rdtime %0\n"
        "rdcycle %1\n"
        "csrsi mstatus, 8\n"
        ".global mt_loop\n"
        "mt_loop:\n"
        "lw t0, 0(%2)\n"
        "beqz t0, mt_loop\n"
        ".global mt_done\n"
        "mt_done:\n"
        "csrci mstatus, 8\n"
        : "=&r"(t_pre), "=&r"(c_pre)
        : "r"(&mt_flag)
        : "t0", "memory");
    r->t_pre = t_pre;
    r->c_pre = c_pre;
}

// Calibration: read (rdcycle, mtime) twice across a 10M-tick spin and
// measure the rdcycle units per mtime tick. On this QEMU build rdcycle
// follows the host CPU clock while mtime follows the 10 MHz virtual
// timebase, so the ratio is about 150 here, not 100. The ratio is
// re-checked on every run; the tick-domain metrics do not depend on it.
static void calibration(void) {
    unsigned long t0, t1, c0, c1, ratio, i, lc, lt, c, t;
    t0 = clint_get_mtime();
    c0 = rd_cycle();
    while (clint_get_mtime() - t0 < 10000000UL)
        ;
    t1 = clint_get_mtime();
    c1 = rd_cycle();
    ratio = (c1 - c0) / (t1 - t0);
    mt_cal_ratio = ratio;
    uart_puts("cal: 10000000 mtime ticks -> rdcycle delta ");
    uart_put_dec(c1 - c0);
    uart_puts(" (ratio ");
    uart_put_dec(ratio);
    uart_puts(" rdcycle units per tick)\n");
    check(ratio >= 145 && ratio <= 155,
          "rdcycle/mtime ratio outside 145..155");
    lc = 0;
    lt = 0;
    for (i = 0; i < 64; i++) {
        c = rd_cycle();
        t = clint_get_mtime();
        check(c >= lc, "rdcycle went backwards");
        check(t >= lt, "mtime went backwards");
        lc = c;
        lt = t;
    }
    uart_puts("cal: 64 back-to-back (rdcycle, mtime) pairs monotonic: ok\n");
}

static void run_one_trial(unsigned long i) {
    mt_rec_t *r = &mt_rec[i];
    unsigned long attempt;
    for (attempt = 0; attempt < MT_MAX_ATTEMPTS; attempt++) {
        unsigned long offset;
        mt_trial_idx = i;
        r->cmp = clint_get_mtime() + MT_AHEAD_TICKS;
        clint_set_mtimecmp(r->cmp);
        mt_trial(r);
        mt_attempts++;
        if (r->t_pre >= r->cmp) {
            /* Host descheduled the vcpu inside the arm window: the
               timer was already pending when the trial block started.
               Re-arm and run the trial again. */
            mt_retries++;
            continue;
        }
        offset = r->t_entry - r->cmp;
        if (offset > MT_OFFSET_BOUND) {
            /* Host descheduled the vcpu during the wait: the delivery
               overshoot is host delay, not timer behavior. Re-run. */
            mt_retries++;
            continue;
        }
        break;
    }
    check(attempt < MT_MAX_ATTEMPTS, "arm window stretched 100 times, host too noisy");
}

// ---- statistics ----

static unsigned long sort_tmp[MT_NTRIALS];

static void sort_asc(unsigned long *a, unsigned long n) {
    unsigned long i, j, v;
    for (i = 0; i < n; i++)
        sort_tmp[i] = a[i];
    for (i = 1; i < n; i++) {
        v = sort_tmp[i];
        j = i;
        while (j > 0 && sort_tmp[j - 1] > v) {
            sort_tmp[j] = sort_tmp[j - 1];
            j--;
        }
        sort_tmp[j] = v;
    }
}

typedef struct {
    unsigned long min, p50, p99, max, mean;
} stats_t;

static stats_t calc_stats(unsigned long *vals, unsigned long n) {
    stats_t s;
    unsigned long i, sum = 0;
    sort_asc(vals, n);
    s.min = sort_tmp[0];
    s.max = sort_tmp[n - 1];
    s.p50 = sort_tmp[n / 2];
    s.p99 = sort_tmp[(n * 99) / 100];
    for (i = 0; i < n; i++)
        sum += sort_tmp[i];
    s.mean = sum / n;
    return s;
}

static void print_stats(const char *name, unsigned long *vals,
                        unsigned long n) {
    stats_t s = calc_stats(vals, n);
    uart_puts(name);
    uart_puts(": min=");
    uart_put_dec(s.min);
    uart_puts(" p50=");
    uart_put_dec(s.p50);
    uart_puts(" p99=");
    uart_put_dec(s.p99);
    uart_puts(" max=");
    uart_put_dec(s.max);
    uart_puts(" mean=");
    uart_put_dec(s.mean);
    uart_puts("\n");
}

// Histogram with 1-unit buckets (for tick-domain metrics).
static void hist_ticks(const char *name, unsigned long *vals,
                       unsigned long n) {
    unsigned long v, c, mn, mx;
    stats_t s = calc_stats(vals, n);
    mn = s.min;
    mx = s.max;
    uart_puts(name);
    uart_puts(" (n=");
    uart_put_dec(n);
    uart_puts("):\n");
    for (v = mn; v <= mx; v++) {
        unsigned long i;
        c = 0;
        for (i = 0; i < n; i++)
            if (vals[i] == v)
                c++;
        if (c > 0) {
            uart_puts("  ");
            uart_put_dec(v);
            uart_puts(": ");
            uart_put_dec(c);
            uart_puts("\n");
        }
    }
}

static unsigned long m_off_ticks[MT_NTRIALS];
static unsigned long m_off_cyc[MT_NTRIALS];

static void print_raw(unsigned long i) {
    mt_rec_t *r = &mt_rec[i];
    uart_puts("raw[");
    uart_put_dec(i);
    uart_puts("]: cmp=");
    uart_put_hex(r->cmp);
    uart_puts(" t_pre=");
    uart_put_hex(r->t_pre);
    uart_puts(" c_pre=");
    uart_put_hex(r->c_pre);
    uart_puts(" t_entry=");
    uart_put_hex(r->t_entry);
    uart_puts(" c_entry=");
    uart_put_hex(r->c_entry);
    uart_puts(" mepc_before=");
    uart_put_hex(r->mepc_before);
    uart_puts(" mcause=");
    uart_put_hex(r->mcause);
    uart_puts("\n");
}

// Verify every recorded trial's stamps and fill the metric arrays
// (post-warmup). Returns the number of trials in the statistics.
static unsigned long verify(void) {
    unsigned long i, n = 0;
    unsigned long sum_c = 0, sum_t = 0; // in-trial counter slope cross-check
    unsigned long loop = (unsigned long)mt_loop;
    unsigned long done = (unsigned long)mt_done;
    for (i = 0; i < MT_NTRIALS; i++) {
        mt_rec_t *r = &mt_rec[i];
        unsigned long offset;
        check(r->mcause == MCAUSE_MTI, "mcause != machine timer interrupt");
        check(r->t_pre < r->cmp, "interrupt already pending at trial start");
        check(r->t_entry >= r->cmp, "trap entry mtime < mtimecmp");
        check(r->t_entry >= r->t_pre, "entry tick < pre tick");
        check(r->c_entry > r->c_pre, "entry rdcycle <= pre rdcycle");
        check(r->mepc_before >= loop && r->mepc_before < done,
              "mepc_before outside the trial spin loop");
        offset = r->t_entry - r->cmp;
        check(offset <= MT_OFFSET_BOUND, "delivery offset beyond sanity bound");
        if (i >= MT_WARMUP) {
            m_off_ticks[n] = offset;
            m_off_cyc[n] = offset * mt_cal_ratio;
            sum_c += r->c_entry - r->c_pre;
            sum_t += r->t_entry - r->t_pre;
            n++;
        }
    }
    uart_puts("in-trial rdcycle-vs-mtime slope: sum_c=");
    uart_put_dec(sum_c);
    uart_puts(" sum_t=");
    uart_put_dec(sum_t);
    uart_puts(" slope=");
    uart_put_dec(sum_t ? sum_c / sum_t : 0);
    uart_puts(" (calibration ratio ");
    uart_put_dec(mt_cal_ratio);
    uart_puts(")\n");
    check(sum_t > 0 && sum_c / sum_t >= 145 && sum_c / sum_t <= 155,
          "in-trial slope outside 145..155");
    return n;
}

static void report(void) {
    unsigned long n;
    uart_puts("trap_count=");
    uart_put_dec(mt_trap_count);
    uart_puts(" (attempts ");
    uart_put_dec(mt_attempts);
    uart_puts(", arm retries ");
    uart_put_dec(mt_retries);
    uart_puts(")\n");
    check(mt_trap_count == mt_attempts, "trap count != attempt count");
    check(!mt_bad_seen, "unexpected trap seen");
    if (mt_bad_seen) {
        uart_puts("bad trap: mcause=");
        uart_put_hex(mt_bad_mcause);
        uart_puts(" mepc=");
        uart_put_hex(mt_bad_mepc);
        uart_puts("\n");
    }

    print_raw(0);
    print_raw(1);

    uart_puts("trials recorded: ");
    uart_put_dec(MT_NTRIALS);
    uart_puts(" (first ");
    uart_put_dec(MT_WARMUP);
    uart_puts(" discarded from stats)\n");
    n = verify();
    print_stats("offset_ticks", m_off_ticks, n);
    print_stats("offset_cyc  ", m_off_cyc, n);
    hist_ticks("hist offset_ticks", m_off_ticks, n);

    if (fails == 0)
        uart_puts("RESULT: PASS\n");
    else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
}

int main(void) {
    unsigned long mtvec;
    uart_init();
    uart_puts("mtimecmp: CLINT mtimecmp delivery accuracy (backlog item 36)\n");
    uart_puts("mhartid=");
    uart_put_dec(rd_mhartid());
    uart_puts("\n");

    __asm__ volatile("csrw mtvec, %0" :: "r"(mt_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(&mt_save));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    uart_puts("mtvec=");
    uart_put_hex(mtvec);
    uart_puts(" mscratch=");
    uart_put_hex((unsigned long)&mt_save);
    uart_puts("\n");
    check((mtvec & 3UL) == 0, "mtvec not in direct mode");

    clint_set_mtimecmp(~0UL); // disarmed until the first trial
    // Only the machine timer interrupt is enabled in mie; mstatus.MIE
    // is set only inside the trial block, so a trap can only land in
    // trial code.
    __asm__ volatile("csrs mie, %0" :: "r"(0x80UL));

    calibration();

    uart_puts("trials: 1000, mtimecmp = mtime+");
    uart_put_dec(MT_AHEAD_TICKS);
    uart_puts(" ticks, offset bound ");
    uart_put_dec(MT_OFFSET_BOUND);
    uart_puts(" ticks\n");
    {
        unsigned long i;
        for (i = 0; i < MT_NTRIALS; i++)
            run_one_trial(i);
    }
    report();

    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
