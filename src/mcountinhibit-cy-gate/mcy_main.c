// mcy_main.c: mcountinhibit.CY gating of mcycle (backlog item
// riscv mcountinhibit-cy-gate).
//
// Mechanism under test: the mcountinhibit CSR stops the gated
// counter from incrementing. With mcountinhibit.CY (bit 0) set,
// mcycle must freeze: 1000 back-to-back csrr reads must all
// return the identical value. With CY cleared again, mcycle
// must resume: samples taken in bounded spins must strictly
// increase. The whole experiment runs in M-mode on hart 0, where
// mcountinhibit is writable; QEMU boots the ELF straight into
// M-mode with -bios none, so no privilege drop is needed.
//
// A minimal M-mode trap handler (mcy_trap.S) records
// mcause/mepc/mtval and a trap count, then parks the hart; any
// trap is unexpected, so reaching the printed PASS implies the
// trap count is zero, and the program also prints and checks the
// count explicitly. On PASS the machine shuts down through the
// virt test-device finisher (QEMU exits 0); on FAIL the hart
// parks in a wfi loop without touching the finisher.

#include "../uart.h"

extern void mcy_trap_entry(void);

// Trap record, written by mcy_trap.S: [0] parked t1,
// [1] mcause, [2] mepc, [3] mtval, [4] trap count.
volatile unsigned long mcy_save[8];

#define FREEZE_N 1000
volatile unsigned long mcy_freeze[FREEZE_N];

#define RESUME_N 4
#define SPIN_BOUND (1UL << 20)

#define MCOUNTINHIBIT_CY (1UL << 0)

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// FNV-1a (64-bit) over the measured samples, fed in a fixed
// order: the 1000 freeze samples, then the resume samples. The
// absolute counter values differ per run (mcycle is a live
// counter), so the checksum is run-specific; it binds the
// printed verdict to the samples this run measured.
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

static unsigned long read_mcycle(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mcycle" : "=r"(v));
    return v;
}

static unsigned long read_mcountinhibit(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mcountinhibit" : "=r"(v));
    return v;
}

static void write_mcountinhibit(unsigned long v) {
    __asm__ volatile("csrw mcountinhibit, %0" ::"r"(v) : "memory");
}

int main(void) {
    unsigned long boot_mci, mcycle_pre, rb, traps;
    unsigned long mism, i, k;
    unsigned long resume[RESUME_N];
    unsigned long timeout = 0;
    int inc;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("mcountinhibit.CY gates mcycle\n");
    uart_puts("CY set must freeze it, CY clear must resume it\n");
    uart_puts("========================================\n\n");

    // Trap vector: any trap parks the hart (mcy_trap.S).
    __asm__ volatile("la t0, mcy_trap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");

    // 1. Boot value, and a live mcycle read proving the
    // counter is running before the gate is applied.
    boot_mci = read_mcountinhibit();
    mcycle_pre = read_mcycle();
    uart_puts("boot: mcountinhibit=");
    uart_put_hex(boot_mci);
    uart_puts(" mcycle(before gate)=");
    uart_put_hex(mcycle_pre);
    uart_puts("\n");

    // 2. Set CY: write, then read back. The readback must
    // carry the bit, proving the write took effect rather
    // than being silently ignored.
    write_mcountinhibit(MCOUNTINHIBIT_CY);
    rb = read_mcountinhibit();
    uart_puts("write: csrw mcountinhibit, 0x1 (CY); readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check((rb & MCOUNTINHIBIT_CY) != 0,
          "mcountinhibit CY write did not read back with CY set");
    cks_feed(rb);

    // 3. Freeze phase: 1000 back-to-back mcycle reads. Every
    // readback must be identical: zero advance while CY is
    // inhibited.
    for (i = 0; i < FREEZE_N; i++)
        mcy_freeze[i] = read_mcycle();
    mism = 0;
    for (i = 1; i < FREEZE_N; i++) {
        if (mcy_freeze[i] != mcy_freeze[0])
            mism++;
        cks_feed(mcy_freeze[i]);
    }
    cks_feed(mcy_freeze[0]);
    uart_puts("freeze: 1000 mcycle reads with CY=1\n");
    uart_puts("  first=");
    uart_put_hex(mcy_freeze[0]);
    uart_puts(" last=");
    uart_put_hex(mcy_freeze[FREEZE_N - 1]);
    uart_puts(" freeze delta (last - first)=");
    uart_put_dec(mcy_freeze[FREEZE_N - 1] - mcy_freeze[0]);
    uart_puts("\n  samples differing from the first: ");
    uart_put_dec(mism);
    uart_puts("\n");
    check(mism == 0, "freeze phase: mcycle advanced while CY inhibited");
    // Note: the frozen value itself is NOT asserted. The
    // mechanism under test is zero advance while CY is
    // inhibited (every readback identical), which is what the
    // check above verifies; the absolute readback while
    // inhibited is QEMU-specific (observed 0x0 on every run)
    // and is reported, not asserted.

    // 4. Clear CY: write 0, read back. The readback must be
    // exactly 0, proving the gate is off.
    write_mcountinhibit(0);
    rb = read_mcountinhibit();
    uart_puts("write: csrw mcountinhibit, 0x0; readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == 0, "mcountinhibit clear did not read back as 0");
    cks_feed(rb);

    // 5. Resume phase: take RESUME_N samples, each one
    // re-read in a bounded spin until it exceeds the
    // previous. Back-to-back reads can land in the same host
    // tick, so the spin waits for the tick; the bound (2^20
    // iterations) is far beyond any real tick interval, and
    // tripping it is a measured failure.
    uart_puts("resume: sampling mcycle with CY=0 (bounded spin per sample)...\n");
    resume[0] = read_mcycle();
    for (k = 1; k < RESUME_N; k++) {
        unsigned long bound = SPIN_BOUND;
        unsigned long v;
        do {
            v = read_mcycle();
            if (--bound == 0) {
                timeout = 1;
                break;
            }
        } while (v <= resume[k - 1]);
        resume[k] = v;
        cks_feed(v);
        if (timeout)
            break;
    }
    cks_feed(resume[0]);
    uart_puts("  samples:");
    for (k = 0; k < RESUME_N; k++) {
        uart_puts(" ");
        uart_put_hex(resume[k]);
    }
    uart_puts("\n  consecutive deltas:");
    for (k = 1; k < RESUME_N; k++) {
        uart_puts(" ");
        uart_put_dec(resume[k] - resume[k - 1]);
    }
    uart_puts("\n  total resume delta (last - first)=");
    uart_put_dec(resume[RESUME_N - 1] - resume[0]);
    uart_puts("\n");
    check(timeout == 0,
          "resume phase: mcycle never advanced (bounded spin tripped)");
    inc = 1;
    for (k = 1; k < RESUME_N; k++) {
        if (resume[k] <= resume[k - 1])
            inc = 0;
    }
    check(inc, "resume phase: samples did not strictly increase");
    check(resume[RESUME_N - 1] > resume[0],
          "resume phase: total delta is not positive");

    // 6. Trap count: the handler parks on the first trap, so
    // reaching here already implies zero, but require it
    // explicitly as well.
    traps = mcy_save[4];
    uart_puts("traps recorded by the handler: ");
    uart_put_dec(traps);
    uart_puts("\n");
    check(traps == 0, "unexpected trap fired during the run");

    uart_puts("\nchecksum (FNV-1a over the measured samples) = ");
    uart_put_hex64(cksum);
    uart_puts("\n");
    uart_puts("checks: ");
    uart_put_dec((unsigned long)nchecks);
    uart_puts("  mismatches: ");
    uart_put_dec((unsigned long)fails);
    uart_puts("\n");

    uart_puts(fails == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;
    }
    for (;;) {
        __asm__ volatile("wfi");
    }
}
