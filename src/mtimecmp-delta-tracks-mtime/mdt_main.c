// mdt_main.c: CLINT mtime advance/delta measurement.
//
// Mechanism under test: mtime is a free-running counter. Two reads
// of mtime with a real delay between them must show a strictly
// positive delta, and repeated pairs must never go backward
// (delta >= 0 on every pair).
//
// The module:
//   1. Reads the CLINT mtime (0x0200bff8) with 32-bit accesses
//      only. 64-bit accesses to CLINT registers fault on this
//      emulator (observed with the msip register in src/msip/ and
//      relied on by src/mtime-write/), so no 64-bit CLINT access
//      is ever issued. A torn read (the counter crossing a 32-bit
//      boundary between the low and high loads) would corrupt the
//      64-bit value, so the read sequence is high, low, high,
//      keeping the pair only when the two high reads agree.
//   2. Runs N_DELAYED pairs: read t0, spin until mtime has
//      advanced at least DELAY_TICKS (bounded by a generous
//      iteration guard so a frozen counter fails instead of
//      hanging), read t1, require t1 > t0, publish t0/t1/delta.
//   3. Runs N_BACKTOBACK pairs: read t0, read t1 immediately,
//      require t1 >= t0, publish the delta. Back-to-back reads
//      can land in the same tick, so a zero delta is possible
//      and is reported, not failed; a negative delta would mean
//      the counter went backward and fails.
//   4. Prints the CLINT addresses read and the trap record, and
//      requires the trap count to be zero. The M-mode trap
//      handler (mdt_trap.S) records and parks on any trap; no
//      trap is expected from plain MMIO loads with interrupts
//      disabled.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop
// without touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.
//
// This run executes in M-mode on hart 0: QEMU boots the ELF
// straight into M-mode with -bios none, so no privilege drop is
// needed. The module shares only src/boot.S and src/uart.c with
// the other demos.

#include "../uart.h"

extern void mdt_trap_entry(void);

// Trap record, written by mdt_trap.S: [0] parked t1,
// [1] mcause, [2] mepc, [3] mtval, [4] trap count.
volatile unsigned long mdt_save[8];

#define CLINT_MTIME_LO ((volatile unsigned int *)0x0200bff8UL)
#define CLINT_MTIME_HI ((volatile unsigned int *)(0x0200bff8UL + 4))

#define N_DELAYED    12     // delayed pairs: t1 > t0 required
#define N_BACKTOBACK 8      // back-to-back pairs: t1 >= t0 required
#define DELAY_TICKS  10000UL  // 1 ms of virtual time at the 10 MHz timebase
#define SPIN_GUARD   (1UL << 28)  // iteration bound; tripping it is a failure

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

// FNV-1a (64-bit) over the measured samples, fed in a fixed
// order: each pair's t0 then t1, then the trap count. The absolute
// counter values differ per run (mtime is a live counter), so the
// checksum is run-specific; it binds the printed verdict to the
// samples this run measured.
static unsigned long long cksum = 1469598103934665603ULL;

static void cks_feed(unsigned long v) {
    int i;
    for (i = 0; i < 8; i++) {
        cksum ^= (unsigned long long)((v >> (8 * i)) & 0xffUL);
        cksum *= 1099511628211ULL;
    }
}

static void uart_put_hex64(unsigned long long v) {
    int i;
    uart_puts("0x");
    for (i = 15; i >= 0; i--) {
        unsigned int d = (unsigned int)((v >> (4 * i)) & 0xfULL);
        uart_putc(d < 10 ? (char)('0' + d) : (char)('a' + d - 10));
    }
}

static int fails = 0;
static int nchecks = 0;

static void check(int cond, const char *msg) {
    nchecks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

int main(void) {
    unsigned long t0, t1, delta;
    unsigned long i, guard;
    unsigned long dmin = ~0UL, dmax = 0, dsum = 0;
    unsigned long bbmin = ~0UL, bbmax = 0, bbzero = 0;
    unsigned long timeout;
    unsigned long traps;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("mtimecmp-delta-tracks-mtime\n");
    uart_puts("CLINT mtime advance/delta measurement\n");
    uart_puts("========================================\n\n");

    // Trap vector: any trap parks the hart (mdt_trap.S).
    __asm__ volatile("la t0, mdt_trap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");

    uart_puts("readback addresses: lo=0x0200bff8 hi=0x0200bffc\n");
    uart_puts("delay: spin until mtime advances ");
    uart_put_dec(DELAY_TICKS);
    uart_puts(" ticks (");
    uart_put_dec(N_DELAYED);
    uart_puts(" delayed pairs, ");
    uart_put_dec(N_BACKTOBACK);
    uart_puts(" back-to-back pairs)\n\n");

    // 1. Delayed pairs: t0, spin for DELAY_TICKS, t1. The second
    // read must be strictly greater; the spin guarantees a real
    // delay, so a non-positive delta would mean the read path did
    // not observe the counter advancing.
    uart_puts("delayed pairs (delta must be > 0):\n");
    for (i = 0; i < N_DELAYED; i++) {
        timeout = 0;
        t0 = read_mtime();
        guard = SPIN_GUARD;
        while (read_mtime() - t0 < DELAY_TICKS) {
            if (--guard == 0) {
                timeout = 1;
                break;
            }
        }
        t1 = read_mtime();
        delta = t1 - t0;
        cks_feed(t0);
        cks_feed(t1);
        uart_puts("  pair ");
        uart_put_dec(i);
        uart_puts(": t0=");
        uart_put_hex(t0);
        uart_puts(" t1=");
        uart_put_hex(t1);
        uart_puts(" delta=");
        uart_put_dec(delta);
        uart_puts("\n");
        if (delta < dmin)
            dmin = delta;
        if (delta > dmax)
            dmax = delta;
        dsum += delta;
        check(!timeout, "delayed pair: spin guard tripped, mtime did not advance");
        check(delta > 0, "delayed pair: second read not greater than first");
    }
    uart_puts("  delayed delta min=");
    uart_put_dec(dmin);
    uart_puts(" max=");
    uart_put_dec(dmax);
    uart_puts(" mean=");
    uart_put_dec(dsum / N_DELAYED);
    uart_puts("\n\n");

    // 2. Back-to-back pairs: t0, t1 immediately. The second read
    // must never be smaller than the first; zero is possible when
    // both reads land in the same tick, and is reported, not
    // failed. The comparison is t1 >= t0 on the raw values: the
    // unsigned delta t1 - t0 would wrap on a backward step and
    // hide it, so the check must not go through the delta.
    uart_puts("back-to-back pairs (t1 must be >= t0):\n");
    for (i = 0; i < N_BACKTOBACK; i++) {
        t0 = read_mtime();
        t1 = read_mtime();
        delta = t1 - t0;
        cks_feed(t0);
        cks_feed(t1);
        uart_puts("  pair ");
        uart_put_dec(i);
        uart_puts(": t0=");
        uart_put_hex(t0);
        uart_puts(" t1=");
        uart_put_hex(t1);
        uart_puts(" delta=");
        uart_put_dec(delta);
        uart_puts("\n");
        if (delta < bbmin)
            bbmin = delta;
        if (delta > bbmax)
            bbmax = delta;
        if (delta == 0)
            bbzero++;
        check(t1 >= t0, "back-to-back pair: counter went backward");
    }

    uart_puts("  back-to-back delta min=");
    uart_put_dec(bbmin);
    uart_puts(" max=");
    uart_put_dec(bbmax);
    uart_puts(" zero-deltas=");
    uart_put_dec(bbzero);
    uart_puts("/");
    uart_put_dec(N_BACKTOBACK);
    uart_puts("\n\n");

    // 3. Trap record: no trap is expected from plain MMIO loads.
    traps = mdt_save[4];
    cks_feed(traps);
    uart_puts("traps: count=");
    uart_put_dec(traps);
    uart_puts(" mcause=");
    uart_put_hex(mdt_save[1]);
    uart_puts(" mepc=");
    uart_put_hex(mdt_save[2]);
    uart_puts(" mtval=");
    uart_put_hex(mdt_save[3]);
    uart_puts("\n");
    check(traps == 0, "a trap fired during plain mtime MMIO reads");

    uart_puts("\nchecks: ");
    uart_put_dec(nchecks);
    uart_puts(", mismatches: ");
    uart_put_dec(fails);
    uart_puts("\nchecksum: ");
    uart_put_hex64(cksum);
    uart_puts("\nRESULT: ");
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
