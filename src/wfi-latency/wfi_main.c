// wfi_main.c: WFI wakeup-latency experiment (proof backlog item 28).
//
// For each trial the program arms the CLINT timer (mtimecmp = mtime +
// WFI_AHEAD_TICKS), then either executes wfi or spins on a flag, with only
// the machine timer interrupt enabled. The trap entry stamps rdtime
// and rdcycle, the C handler verifies the cause, disarms the timer and
// resumes at the trial's resume label, and the first two instructions
// after mret stamp rdcycle and rdtime again.
//
// What is measured per trial, all in the tick domain unless noted:
//   wake_to_entry  = t_entry - cmp      (programmed wake to trap entry)
//   entry_to_resume = t_resume - t_entry (trap path, ticks)
//   wake_to_resume = t_resume - cmp      (programmed wake to first task
//                                        instruction; the headline number)
//   trap_cyc       = c_resume - c_entry  (trap path, rdcycle units: the
//                                        fine-grained view; on this QEMU
//                                        build rdcycle advances at the
//                                        host CPU rate, about 149.75
//                                        units per mtime tick, verified
//                                        by the calibration below)
//
// A spin baseline runs the identical trap path with the hart spinning
// instead of sleeping in wfi, so any wfi-specific cost shows up as a
// difference between the two distributions.
//
// If the host deschedules the vcpu inside the arm-to-trial window, the
// timer is already pending when the trial block starts (t_pre >= cmp).
// The trial is then re-armed and re-run (up to 100 attempts); only trials
// with a clean arm are recorded, and the retry count is reported.

#include "wfi.h"
#include "../preempt/clint.h"
#include "../uart.h"

wfi_save_t wfi_save;
trial_rec_t wfi_rec[WFI_NTRIALS];
trial_rec_t spin_rec[WFI_NTRIALS];
volatile unsigned long wfi_current_kind;
volatile unsigned long wfi_trial_idx;
volatile unsigned long wfi_spin_flag;
volatile unsigned long wfi_trap_count;

static unsigned long wfi_bad_seen;
static unsigned long wfi_bad_mcause;
static unsigned long wfi_bad_mepc;

static int fails = 0;

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
// not immediately re-trap, and resumes at the trial's resume label so
// the first instruction after mret is the rdcycle stamp.
void wfi_trap_handler(wfi_save_t *s) {
    unsigned long k = wfi_current_kind;
    unsigned long i = wfi_trial_idx;
    trial_rec_t *r;

    if (s->mcause != MCAUSE_MTI || i >= WFI_NTRIALS) {
        wfi_bad_seen = 1;
        wfi_bad_mcause = s->mcause;
        wfi_bad_mepc = s->mepc;
        clint_set_mtimecmp(~0UL);
        s->mepc = (k == 0) ? (unsigned long)wfi_resume
                           : (unsigned long)spin_resume;
        return;
    }
    r = (k == 0) ? &wfi_rec[i] : &spin_rec[i];
    r->mcause = s->mcause;
    r->mepc_before = s->mepc;
    r->t_entry = s->mtime_entry;
    r->c_entry = s->cycle_entry;
    clint_set_mtimecmp(~0UL);
    wfi_spin_flag = 1;
    s->mepc = (k == 0) ? (unsigned long)wfi_resume
                       : (unsigned long)spin_resume;
    wfi_trap_count++;
}

// One wfi trial. The asm block is emitted verbatim: rdtime, rdcycle,
// enable MIE, wfi, then the resume label with rdcycle and rdtime. The
// handler always resumes at wfi_resume, so c_resume is stamped by the
// first instruction after mret regardless of where the trap found mepc
// (at the wfi or after it). Marked noinline so the global label is
// defined exactly once.
__attribute__((noinline)) static void wfi_trial(trial_rec_t *r) {
    unsigned long t_pre, c_pre, c_resume, t_resume;
    __asm__ volatile(
        "rdtime %0\n"
        "rdcycle %1\n"
        "csrsi mstatus, 8\n"
        "wfi\n"
        ".global wfi_resume\n"
        "wfi_resume:\n"
        "rdcycle %2\n"
        "rdtime %3\n"
        : "=&r"(t_pre), "=&r"(c_pre), "=&r"(c_resume), "=&r"(t_resume)
        :
        : "memory");
    __asm__ volatile("csrci mstatus, 8" ::: "memory");
    r->t_pre = t_pre;
    r->c_pre = c_pre;
    r->c_resume = c_resume;
    r->t_resume = t_resume;
}

// One spin-baseline trial: identical trap path, but the hart spins on
// wfi_spin_flag (set by the handler) instead of sleeping in wfi. The
// handler resumes at spin_resume, so the resume stamps are taken at the
// same structural point as in the wfi trial.
__attribute__((noinline)) static void spin_trial(trial_rec_t *r) {
    unsigned long t_pre, c_pre, c_resume, t_resume;
    wfi_spin_flag = 0;
    __asm__ volatile(
        ".global spin_loop\n"
        "spin_loop:\n"
        "rdtime %0\n"
        "rdcycle %1\n"
        "csrsi mstatus, 8\n"
        "1:\n"
        "lw t0, 0(%4)\n"
        "beqz t0, 1b\n"
        ".global spin_resume\n"
        "spin_resume:\n"
        "rdcycle %2\n"
        "rdtime %3\n"
        : "=&r"(t_pre), "=&r"(c_pre), "=&r"(c_resume), "=&r"(t_resume)
        : "r"(&wfi_spin_flag)
        : "t0", "memory");
    __asm__ volatile("csrci mstatus, 8" ::: "memory");
    r->t_pre = t_pre;
    r->c_pre = c_pre;
    r->c_resume = c_resume;
    r->t_resume = t_resume;
}

// Calibration: spin for 10M mtime ticks and measure the rdcycle delta.
// This verifies both counters advance and pins down the rdcycle units
// per tick on this QEMU build (about 149.75 here: rdcycle follows the
// host CPU clock, mtime the 10 MHz virtual timebase). The ratio is
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

static unsigned long arm_retries;

static void run_one_trial(trial_rec_t *r, unsigned long i, int kind) {
    unsigned long attempt;
    for (attempt = 0; attempt < 100; attempt++) {
        wfi_current_kind = kind;
        wfi_trial_idx = i;
        r->cmp = clint_get_mtime() + WFI_AHEAD_TICKS;
        clint_set_mtimecmp(r->cmp);
        if (kind == 0)
            wfi_trial(r);
        else
            spin_trial(r);
        if (r->t_pre < r->cmp)
            break;
        /* Host descheduled the vcpu inside the arm-to-trial window, so the
           timer was already pending when the trial block started. Re-arm and
           run the trial again; the stale record is overwritten. */
        arm_retries++;
    }
    check(attempt < 100, "arm window stretched 100 times, host too noisy");
}

static void run_trials(void) {
    unsigned long i;
    for (i = 0; i < WFI_NTRIALS; i++) {
        run_one_trial(&wfi_rec[i], i, 0);
        run_one_trial(&spin_rec[i], i, 1);
    }
}

// ---- statistics ----

static unsigned long sort_tmp[WFI_NTRIALS];

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
    unsigned long min, p50, max, mean;
} stats_t;

static stats_t calc_stats(unsigned long *vals, unsigned long n) {
    stats_t s;
    unsigned long i, sum = 0;
    sort_asc(vals, n);
    s.min = sort_tmp[0];
    s.max = sort_tmp[n - 1];
    s.p50 = sort_tmp[n / 2];
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

// Histogram with fixed-width buckets (for rdcycle-domain metrics).
static void hist_cyc(const char *name, unsigned long *vals,
                     unsigned long n, unsigned long width) {
    unsigned long b, lo, hi, c, i;
    stats_t s = calc_stats(vals, n);
    lo = (s.min / width) * width;
    hi = s.max;
    uart_puts(name);
    uart_puts(" bucket=");
    uart_put_dec(width);
    uart_puts(" (n=");
    uart_put_dec(n);
    uart_puts("):\n");
    for (b = lo; b <= hi; b += width) {
        c = 0;
        for (i = 0; i < n; i++)
            if (vals[i] >= b && vals[i] < b + width)
                c++;
        if (c > 0) {
            uart_puts("  ");
            uart_put_dec(b);
            uart_puts("-");
            uart_put_dec(b + width - 1);
            uart_puts(": ");
            uart_put_dec(c);
            uart_puts("\n");
        }
    }
}

static unsigned long m_w2r[WFI_NTRIALS];
static unsigned long m_w2e[WFI_NTRIALS];
static unsigned long m_e2r[WFI_NTRIALS];
static unsigned long m_cyc[WFI_NTRIALS];

static void print_raw(const char *tag, trial_rec_t *r, unsigned long i) {
    uart_puts("raw ");
    uart_puts(tag);
    uart_puts("[");
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
    uart_puts(" t_resume=");
    uart_put_hex(r->t_resume);
    uart_puts(" c_resume=");
    uart_put_hex(r->c_resume);
    uart_puts(" mepc_before=");
    uart_put_hex(r->mepc_before);
    uart_puts(" mcause=");
    uart_put_hex(r->mcause);
    uart_puts("\n");
}

// Verify every trial's stamps and fill the metric arrays (post-warmup).
static unsigned long verify_kind(const char *tag, trial_rec_t *rec,
                                int is_wfi) {
    unsigned long i, n = 0;
    unsigned long resume = (unsigned long)(is_wfi ? wfi_resume : spin_resume);
    unsigned long loop = (unsigned long)spin_loop;
    unsigned long at_wfi = 0, after_wfi = 0, other = 0;
    for (i = 0; i < WFI_NTRIALS; i++) {
        trial_rec_t *r = &rec[i];
        check(r->mcause == MCAUSE_MTI, "mcause != machine timer interrupt");
        check(r->t_pre < r->cmp, "interrupt already pending at trial start");
        check(r->t_entry >= r->cmp, "trap entry mtime < mtimecmp");
        check(r->t_resume >= r->t_entry, "resume tick < entry tick");
        check(r->c_entry > r->c_pre, "entry rdcycle <= pre rdcycle");
        check(r->c_resume > r->c_entry, "resume rdcycle <= entry rdcycle");
        if (is_wfi) {
            if (r->mepc_before == resume - 4)
                at_wfi++;
            else if (r->mepc_before == resume)
                after_wfi++;
            else {
                other++;
                check(0, "wfi mepc_before not at/after wfi");
            }
        } else {
            check(r->mepc_before >= loop && r->mepc_before < resume,
                  "spin mepc_before outside spin loop");
        }
        if (i >= WFI_WARMUP) {
            m_w2r[n] = r->t_resume - r->cmp;
            m_w2e[n] = r->t_entry - r->cmp;
            m_e2r[n] = r->t_resume - r->t_entry;
            m_cyc[n] = r->c_resume - r->c_entry;
            n++;
        }
    }
    uart_puts(tag);
    uart_puts(": mepc_before at_wfi=");
    uart_put_dec(at_wfi);
    uart_puts(" after_wfi=");
    uart_put_dec(after_wfi);
    uart_puts(" other=");
    uart_put_dec(other);
    uart_puts("\n");
    return n;
}

static void report(void) {
    unsigned long n;
    uart_puts("trap_count=");
    uart_put_dec(wfi_trap_count);
    uart_puts(" (expected ");
    uart_put_dec(2UL * WFI_NTRIALS + arm_retries);
    uart_puts(", arm retries=");
    uart_put_dec(arm_retries);
    uart_puts(")\n");
    check(wfi_trap_count == 2UL * WFI_NTRIALS + arm_retries, "trap count mismatch");
    check(!wfi_bad_seen, "unexpected trap seen");
    if (wfi_bad_seen) {
        uart_puts("bad trap: mcause=");
        uart_put_hex(wfi_bad_mcause);
        uart_puts(" mepc=");
        uart_put_hex(wfi_bad_mepc);
        uart_puts("\n");
    }

    print_raw("wfi", &wfi_rec[0], 0);
    print_raw("wfi", &wfi_rec[1], 1);
    print_raw("spin", &spin_rec[0], 0);
    print_raw("spin", &spin_rec[1], 1);

    uart_puts("--- wfi ---\n");
    n = verify_kind("wfi", wfi_rec, 1);
    print_stats("wake_to_resume_ticks ", m_w2r, n);
    print_stats("wake_to_entry_ticks  ", m_w2e, n);
    print_stats("entry_to_resume_ticks", m_e2r, n);
    print_stats("trap_rdcycle_units   ", m_cyc, n);
    hist_ticks("hist wake_to_resume_ticks", m_w2r, n);
    hist_cyc("hist trap_rdcycle_units", m_cyc, n, 150);

    uart_puts("--- spin baseline ---\n");
    n = verify_kind("spin", spin_rec, 0);
    print_stats("wake_to_resume_ticks ", m_w2r, n);
    print_stats("wake_to_entry_ticks  ", m_w2e, n);
    print_stats("entry_to_resume_ticks", m_e2r, n);
    print_stats("trap_rdcycle_units   ", m_cyc, n);
    hist_ticks("hist wake_to_resume_ticks", m_w2r, n);
    hist_cyc("hist trap_rdcycle_units", m_cyc, n, 150);

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
    uart_puts("wfi-latency: CLINT timer wakeup latency (backlog item 28)\n");
    uart_puts("mhartid=");
    uart_put_dec(rd_mhartid());
    uart_puts("\n");

    __asm__ volatile("csrw mtvec, %0" :: "r"(wfi_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(&wfi_save));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    uart_puts("mtvec=");
    uart_put_hex(mtvec);
    uart_puts(" mscratch=");
    uart_put_hex((unsigned long)&wfi_save);
    uart_puts("\n");
    check((mtvec & 3UL) == 0, "mtvec not in direct mode");

    clint_set_mtimecmp(~0UL); // disarmed until the first trial
    // Only the machine timer interrupt is enabled in mie; mstatus.MIE
    // is set only inside the trial windows, so a trap can only land in
    // trial code.
    __asm__ volatile("csrs mie, %0" :: "r"(0x80UL));

    calibration();

    uart_puts("trials: 400 wfi + 400 spin, mtimecmp = mtime+");
    uart_put_dec(WFI_AHEAD_TICKS);
    uart_puts(" ticks, first 8 of each discarded\n");
    run_trials();
    report();

    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
