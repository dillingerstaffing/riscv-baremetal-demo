// baud_main.c: measure the bit timing implied by the ns16550a divisor
// latch on the QEMU virt board.
//
// How the measurement works, from the hardware model up:
//   - The guest programs the UART divisor latch (DLAB dance), enables
//     the 16550A FIFOs, and switches the UART to internal loopback.
//   - It writes one byte to THR. The byte loops back into the receive
//     FIFO, which arms QEMU's receive FIFO timeout for exactly 4
//     character times (40 bit times at 8N1: 1 start + 8 data + 1 stop).
//   - The guest stamps rdcycle before the THR write and again when the
//     character-timeout indication (IIR = 0x0C) appears, then drains the
//     byte. One timeout per run: every cost inside the window is a
//     one-off (the THR-write exit that arms the timer, one poll
//     iteration that observes it), each a few microseconds and measured
//     separately below, so the window is essentially the timeout itself.
//   - rdcycle deltas become time via a calibration against the CLINT
//     mtime, which the virt device tree documents at 10 MHz.
//   - Measured bit time = interval / 40, compared against the nominal
//     bit time for the programmed divisor.
//
// What this deliberately does NOT use: the THRE/TEMT status bits. On
// this QEMU the transmitter completes synchronously inside the THR
// write (serial_xmit runs in the MMIO write path and sets both bits
// before returning), so they carry no baud timing. A one-shot check
// below records the LSR value immediately after a THR write to show
// this directly.
//
// Nominal bit time: the virt board wires the ns16550a model with
// baudbase 399193 (hw/riscv/virt.c passes 399193 to serial_mm_init),
// so the model derives bit_time = divisor / 399193 seconds. That is
// not the textbook 1.8432 MHz / 115200 figure; the runs below confirm
// which one the model implements by checking measured_bit / divisor
// is constant across divisors.
//
// Known imperfection, stated up front: QEMU's virtual-clock timers are
// processed by the main-loop thread, so on a loaded host the timeout
// can be delivered late by tens of microseconds to milliseconds. Every
// such delay is positive, so the minimum over runs is the estimate
// closest to the model's true timeout, and the spread is reported.

#include "../uart.h"

#define UART0_BASE 0x10000000UL

#define R_THR 0x00  // write: transmit holding; read: receive buffer
#define R_IER 0x01
#define R_FCR 0x02  // write: FIFO control
#define R_IIR 0x02  // read: interrupt ident
#define R_LCR 0x03
#define R_MCR 0x04
#define R_LSR 0x05

#define LSR_DR   0x01
#define LSR_THRE 0x20
#define LSR_TEMT 0x40

#define LCR_DLAB 0x80
#define MCR_LOOP 0x10
#define FCR_ENABLE 0x01
#define FCR_RX_CLR 0x02
#define FCR_TX_CLR 0x04
#define IER_RDI 0x01

#define IIR_CTI 0x0c

#define CLINT_MTIME 0x0200bff8UL

// Baudbase the virt board gives the ns16550a model (see note above).
#define QEMU_BAUDBASE 399193UL
// Bit times per FIFO timeout at 8N1: 4 character times of 10 bits.
#define BITS_PER_TIMEOUT 40UL

#define NDIV 3
#define NRUN 12

static const unsigned long divisors[NDIV] = {1, 12, 96};

static volatile unsigned char *ureg(unsigned long off) {
    return (volatile unsigned char *)(UART0_BASE + off);
}

static unsigned char uget(unsigned long off) {
    return *ureg(off);
}

static void uset(unsigned long off, unsigned char v) {
    *ureg(off) = v;
}

static unsigned long rdcycle(void) {
    unsigned long c;
    __asm__ volatile("rdcycle %0" : "=r"(c) :: "memory");
    return c;
}

static unsigned long rdtime(void) {
    unsigned long t;
    __asm__ volatile("rdtime %0" : "=r"(t) :: "memory");
    return t;
}

static unsigned long long mtime_get(void) {
    return *(volatile unsigned long long *)CLINT_MTIME;
}

static void put_sdec(long v) {
    if (v < 0) {
        uart_putc('-');
        v = -v;
    }
    uart_put_dec((unsigned long)v);
}

// ---- measurement state (BSS, cleared by boot.S) ----
static unsigned long cal_dtick;    // mtime ticks in calibration window
static unsigned long cal_dcycle;   // rdcycle ticks in same window
static unsigned long cps;          // derived rdcycle units per second
static unsigned long mmio_min;     // fastest single LSR read, cycles
static unsigned long mmio_mean;    // mean single LSR read, cycles
static unsigned long wr_min[NDIV]; // fastest single THR write, cycles
static unsigned long wr_mean[NDIV];// mean single THR write, cycles
static unsigned long sync_lsr[NDIV];  // LSR sampled right after THR write
static unsigned long sync_cyc[NDIV];  // cycles from write to that sample
static unsigned long run_cyc[NDIV][NRUN];  // cycles per single-timeout run
static int run_timed_out[NDIV][NRUN];

static void uart_divisor(unsigned long d) {
    uset(R_LCR, 0x83);  // DLAB=1, 8N1 framing
    uset(R_THR, (unsigned char)(d & 0xff));         // divisor latch low
    uset(R_IER, (unsigned char)((d >> 8) & 0xff));  // divisor latch high
    uset(R_LCR, 0x03);  // DLAB=0, 8N1 framing
}

static void test_mode_on(void) {
    uset(R_FCR, FCR_ENABLE | FCR_RX_CLR | FCR_TX_CLR);
    uset(R_MCR, MCR_LOOP);
    // The 16550 reports the character timeout through the received-data
    // interrupt path, so IER_RDI must be set for IIR to show 0x0C.
    // Verified empirically: with IER=0 the timeout never appears in IIR.
    uset(R_IER, IER_RDI);
    while (uget(R_LSR) & LSR_DR)
        (void)uget(R_THR);  // drain anything stale
}

static void test_mode_off(void) {
    uset(R_MCR, 0x00);
    uset(R_FCR, 0x00);
    uset(R_IER, 0x00);
    uset(R_LCR, 0x03);
}

// Nominal bit time in picoseconds for divisor d.
static unsigned long nominal_ps(unsigned long d) {
    return d * 1000000000000UL / QEMU_BAUDBASE;
}

// Convert a cycle count to picoseconds using the mtime calibration:
// one mtime tick is 100 ns (10 MHz), so ps = cyc * dtick * 100 * 1000
// / dcycle. All intermediate values fit in 64 bits for our ranges.
static unsigned long cyc_to_ps(unsigned long cyc) {
    return cyc * (cal_dtick * 100UL) * 1000UL / cal_dcycle;
}

// Expected rdcycle count for one FIFO timeout at divisor d, used only
// to bound the poll loop so a stuck test fails loudly instead of
// hanging the board.
static unsigned long expected_cyc(unsigned long d) {
    return BITS_PER_TIMEOUT * d * cps / QEMU_BAUDBASE;
}

int main(void) {
    uart_init();

    uart_puts("\n========================================\n");
    uart_puts("riscv-baremetal-demo: uart baud timing\n");
    uart_puts("divisor latch vs measured bit time\n");
    uart_puts("========================================\n\n");

    // ---- calibration: rdcycle against mtime ----
    // 400 ms window: rdcycle's host-derived rate jitters on short
    // windows, so average over a long one.
    unsigned long long m0 = mtime_get();
    unsigned long c0 = rdcycle();
    while (mtime_get() - m0 < 4000000)
        ;
    unsigned long long m1 = mtime_get();
    unsigned long c1 = rdcycle();
    cal_dtick = (unsigned long)(m1 - m0);
    cal_dcycle = c1 - c0;
    // cycles per second = dcycle * 10^7 / dtick (dtick ticks of 100 ns)
    cps = cal_dcycle * 10000000UL / cal_dtick;

    uart_puts("cal: mtime ticks="); uart_put_dec(cal_dtick);
    uart_puts(" rdcycle ticks="); uart_put_dec(cal_dcycle);
    uart_puts("\ncal: rdcycle units per second="); uart_put_dec(cps);
    uart_puts(" (mtime is 10 MHz per virt device tree)\n");

    // rdtime should track the mtime MMIO register.
    unsigned long long ma = mtime_get();
    unsigned long ta = rdtime();
    unsigned long long mb = mtime_get();
    uart_puts("cal: mtime="); uart_put_dec((unsigned long)ma);
    uart_puts(" rdtime="); uart_put_dec(ta);
    uart_puts(" mtime="); uart_put_dec((unsigned long)mb);
    uart_puts("\n");

    // Cost of one MMIO status read: bounds the poll observation latency.
    unsigned long best = ~0UL, sum = 0;
    for (int i = 0; i < 32; i++) {
        unsigned long a = rdcycle();
        (void)uget(R_LSR);
        unsigned long b = rdcycle();
        unsigned long d = b - a;
        sum += d;
        if (d < best)
            best = d;
    }
    mmio_min = best;
    mmio_mean = sum / 32;
    uart_puts("cal: one LSR read min="); uart_put_dec(mmio_min);
    uart_puts(" mean="); uart_put_dec(mmio_mean);
    uart_puts(" cycles\n\n");

    // ---- per-divisor measurements (no console output from here until
    // the report: the UART is in loopback, so any transmit would be
    // swallowed by the receiver instead of reaching the terminal) ----
    for (int di = 0; di < NDIV; di++) {
        unsigned long d = divisors[di];

        uart_divisor(d);
        test_mode_on();

        // Transmit-path check: sample LSR immediately after a THR
        // write. If the transmitter were paced by the baud clock,
        // THRE/TEMT would still be clear here (at divisor 96 one char
        // time is ~2.4 ms; this sample lands microseconds after the
        // write).
        unsigned long s0 = rdcycle();
        uset(R_THR, 0x55);
        sync_lsr[di] = uget(R_LSR);
        sync_cyc[di] = rdcycle() - s0;
        (void)uget(R_THR);  // drain the looped-back byte

        // Cost of one THR write: bounds the stamp-to-arm one-off cost.
        // Bytes accumulate in the receive FIFO; overrun past 16 is
        // harmless for a cost probe. FCR reset afterwards drops them.
        best = ~0UL;
        sum = 0;
        for (int i = 0; i < 32; i++) {
            unsigned long a = rdcycle();
            uset(R_THR, 0x55);
            unsigned long b = rdcycle();
            unsigned long dd = b - a;
            sum += dd;
            if (dd < best)
                best = dd;
        }
        wr_min[di] = best;
        wr_mean[di] = sum / 32;
        uset(R_FCR, FCR_ENABLE | FCR_RX_CLR | FCR_TX_CLR);

        // Timed runs: one FIFO timeout each.
        for (int ri = 0; ri < NRUN; ri++) {
            int timed_out = 0;
            unsigned long t0 = rdcycle();
            uset(R_THR, 0x55);
            unsigned long deadline = rdcycle() + 20 * expected_cyc(d);
            for (;;) {
                unsigned char iir = uget(R_IIR) & 0x0f;
                if (iir == IIR_CTI)
                    break;
                if (rdcycle() > deadline) {
                    timed_out = 1;
                    break;
                }
            }
            run_cyc[di][ri] = rdcycle() - t0;
            run_timed_out[di][ri] = timed_out;
            // Drain outside the timed window: the next run re-arms from
            // a clean receiver.
            while (uget(R_LSR) & LSR_DR)
                (void)uget(R_THR);
            uset(R_FCR, FCR_ENABLE | FCR_RX_CLR | FCR_TX_CLR);
        }
    }
    test_mode_off();

    // ---- report ----
    uart_puts("nominal: bit_time = divisor / 399193 s (virt board baudbase)\n");
    uart_puts("method: one FIFO rx timeout (40 bit times) per run\n\n");

    for (int di = 0; di < NDIV; di++) {
        unsigned long d = divisors[di];
        unsigned long nom = nominal_ps(d);

        uart_puts("divisor "); uart_put_dec(d);
        uart_puts(": sync-xmit LSR="); uart_put_hex(sync_lsr[di]);
        uart_puts(" (sampled "); uart_put_dec(sync_cyc[di]);
        uart_puts(" cyc after THR write)\n");
        uart_puts("divisor "); uart_put_dec(d);
        uart_puts(": THR write min="); uart_put_dec(wr_min[di]);
        uart_puts(" mean="); uart_put_dec(wr_mean[di]);
        uart_puts(" cyc\n");
        uart_puts("divisor "); uart_put_dec(d);
        uart_puts(": nominal bit "); uart_put_dec(nom);
        uart_puts(" ps, "); uart_put_dec(NRUN);
        uart_puts(" runs\n");

        unsigned long mn = ~0UL, mx = 0, msum = 0;
        long pmn = 0, pmx = 0, psum = 0;
        int valid = 0;
        for (int ri = 0; ri < NRUN; ri++) {
            if (run_timed_out[di][ri]) {
                uart_puts("  run "); uart_put_dec((unsigned long)ri);
                uart_puts(": TIMEOUT (no CTI within 20x expected)\n");
                continue;
            }
            unsigned long bit = cyc_to_ps(run_cyc[di][ri]) / BITS_PER_TIMEOUT;
            long ppm = (long)bit - (long)nom;
            ppm = ppm * 1000000L / (long)nom;
            uart_puts("  run "); uart_put_dec((unsigned long)ri);
            uart_puts(": bit "); uart_put_dec(bit);
            uart_puts(" ps, ppm "); put_sdec(ppm);
            uart_puts("\n");
            if (bit < mn) mn = bit;
            if (bit > mx) mx = bit;
            msum += bit;
            if (valid == 0 || ppm < pmn) pmn = ppm;
            if (valid == 0 || ppm > pmx) pmx = ppm;
            psum += ppm;
            valid++;
        }
        if (valid > 0) {
            uart_puts("  bit ps min "); uart_put_dec(mn);
            uart_puts(" max "); uart_put_dec(mx);
            uart_puts(" mean "); uart_put_dec(msum / (unsigned long)valid);
            uart_puts("\n  bit/divisor "); uart_put_dec(msum / (unsigned long)valid / d);
            uart_puts(" ps (2505053 matches baudbase 399193)\n");
            uart_puts("  ppm min "); put_sdec(pmn);
            uart_puts(" max "); put_sdec(pmx);
            uart_puts(" mean "); put_sdec(psum / valid);
            uart_puts(" (one-off costs inside each run: one THR write + one poll iteration, see above)\n");
        }
        uart_puts("\n");
    }

    uart_puts("done\n");
    return 0;
}
