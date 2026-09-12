// rw_main.c: CLINT mtimecmp read/write register probe.
//
// Mechanism under test: on the QEMU virt board, the CLINT
// mtimecmp register for hart 0 (0x02004000) is a plain
// read/write MMIO register. A written 64-bit pattern must read
// back exactly, and the write itself must neither trap nor
// deliver an interrupt.
//
// The module:
//   1. Boots straight into M-mode on hart 0 (QEMU -bios none) and
//      installs the M-mode trap handler (rw_trap.S) that records
//      mcause/mepc/mtval and a trap count, then parks the hart.
//      Any trap during the run is a failure.
//   2. Writes 0 to the mie CSR so every machine interrupt enable
//      bit is clear, including mie.MTIE. A pending machine timer
//      interrupt can therefore not be delivered, even when the
//      CLINT hardware sets mip.MTIP.
//   3. Writes three patterns to mtimecmp with two 32-bit stores
//      (low word, then high word): 0x0, 0x123456789ABCDEF0, and
//      0xFFFFFFFFFFFFFFFF. 64-bit CLINT accesses are never issued:
//      they fault on this emulator (observed with the msip
//      register in src/msip/, and relied on by
//      src/mtimecmp-delta-tracks-mtime/ and src/mtime-write/), so
//      the 32-bit form is used throughout. mtimecmp is not a
//      free-running counter, so the lo/hi split cannot tear; the
//      register holds the written value after both stores.
//   4. Reads each pattern back with two 32-bit loads and requires
//      exact equality, and samples mip after each write to record
//      the MTIP bit honestly. Writing 0x0 while mtime is running
//      makes mtime >= mtimecmp, so MTIP pends; with MTIE clear no
//      trap fires, and that pending-but-undelivered state is the
//      expected honest result. The two larger patterns leave
//      mtimecmp far above the running mtime, so MTIP is clear.
//   5. Writes mtimecmp back to all-ones (the disarmed state),
//      publishes every write/readback pair, the trap record, and
//      the mip observations, and requires the trap count to be
//      zero.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop
// without touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.
//
// The module shares only src/boot.S and src/uart.c with the other
// demos.

#include "../uart.h"

extern void rw_trap_entry(void);

// Trap record, written by rw_trap.S: [0] parked t1,
// [1] mcause, [2] mepc, [3] mtval, [4] trap count.
volatile unsigned long rw_save[8];

#define CLINT_MTIMECMP_LO ((volatile unsigned int *)0x02004000UL)
#define CLINT_MTIMECMP_HI ((volatile unsigned int *)(0x02004000UL + 4))

#define MIP_MTIP (1UL << 7)

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// Write mtimecmp as two 32-bit stores: low word first, then the
// high word. The register holds the written value after both
// stores complete; mtimecmp is not a live counter, so the split
// cannot tear. Interrupts are disabled by mie=0, so nothing else
// observes the intermediate value anyway.
static void write_mtimecmp(unsigned long v) {
    *CLINT_MTIMECMP_LO = (unsigned int)(v & 0xffffffffUL);
    *CLINT_MTIMECMP_HI = (unsigned int)(v >> 32);
}

// Read mtimecmp back with two 32-bit loads: low word, then high.
static unsigned long read_mtimecmp(void) {
    unsigned long lo, hi;
    lo = *CLINT_MTIMECMP_LO;
    hi = *CLINT_MTIMECMP_HI;
    return (hi << 32) | lo;
}

static unsigned long read_mip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mip" : "=r"(v));
    return v;
}

static unsigned long read_mie(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mie" : "=r"(v));
    return v;
}

static unsigned long rd_cycle(void) {
    unsigned long v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

// FNV-1a (64-bit) over the run's fixed and measured data, fed in a
// fixed order: each trial's written value, readback, and mip
// sample, then the final disarm readback and the trap count. Every
// input is deterministic for a passing run (no live mtime values
// are fed in), so the checksum is identical across runs; it binds
// the printed verdict to the values this run measured.
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

// The three written patterns.
static const unsigned long patterns[3] = {
    0x0000000000000000UL,
    0x123456789abcdef0UL,
    0xffffffffffffffffUL,
};

int main(void) {
    unsigned long written, readback, mip;
    unsigned long traps;
    int i;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("mtimecmp-rw\n");
    uart_puts("CLINT mtimecmp read/write register probe\n");
    uart_puts("========================================\n\n");

    // Trap vector: any trap records and parks the hart (rw_trap.S).
    __asm__ volatile("la t0, rw_trap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");

    // All mie bits clear for the whole run, so no machine
    // interrupt can be delivered. This is what lets the 0x0 write
    // pend mip.MTIP without taking a timer trap.
    __asm__ volatile("csrw mie, zero" : : : "memory");
    check(read_mie() == 0, "mie was not fully clear");
    uart_puts("mie: all bits clear (MTIE=0), so no machine interrupt can be delivered\n");
    uart_puts("mtimecmp address: 0x02004000 (hart 0, virt CLINT)\n\n");

    uart_puts("write/readback trials (each requires exact readback):\n");
    for (i = 0; i < 3; i++) {
        written = patterns[i];
        write_mtimecmp(written);
        readback = read_mtimecmp();
        mip = read_mip();
        cks_feed(written);
        cks_feed(readback);
        cks_feed(mip);
        uart_puts("  trial ");
        uart_put_dec((unsigned long)i);
        uart_puts(": wrote=");
        uart_put_hex64(written);
        uart_puts(" read=");
        uart_put_hex64(readback);
        uart_puts(" equal=");
        uart_puts(readback == written ? "yes" : "no");
        uart_puts(" mip.MTIP=");
        uart_puts((mip & MIP_MTIP) ? "1 (pending)" : "0 (clear)");
        uart_puts("\n");
        check(readback == written, "mtimecmp readback did not equal the written pattern");
    }
    uart_puts("\n");

    // Disarm: mtimecmp back to all-ones, and confirm the disarmed
    // state reads back too.
    write_mtimecmp(~0UL);
    readback = read_mtimecmp();
    cks_feed(readback);
    uart_puts("final disarm: wrote=0xffffffffffffffff read=");
    uart_put_hex64(readback);
    uart_puts(" mip.MTIP=");
    mip = read_mip();
    uart_puts((mip & MIP_MTIP) ? "1 (pending)" : "0 (clear)");
    uart_puts("\n");
    check(readback == ~0UL, "final disarm write did not read back all-ones");
    uart_puts("\n");

    // Trap record: no trap is expected from plain MMIO stores and
    // loads with mie=0.
    traps = rw_save[4];
    cks_feed(traps);
    uart_puts("traps: count=");
    uart_put_dec(traps);
    uart_puts(" mcause=");
    uart_put_hex(rw_save[1]);
    uart_puts(" mepc=");
    uart_put_hex(rw_save[2]);
    uart_puts(" mtval=");
    uart_put_hex(rw_save[3]);
    uart_puts("\n");
    check(traps == 0, "a trap fired during mtimecmp MMIO writes/reads");

    uart_puts("\nchecks: ");
    uart_put_dec((unsigned long)nchecks);
    uart_puts(", mismatches: ");
    uart_put_dec((unsigned long)fails);
    uart_puts("\nchecksum: ");
    uart_put_hex64(cksum);
    uart_puts("\nRESULT: ");
    uart_puts(fails == 0 ? "PASS" : "FAIL");
    uart_puts("\n");

    // Let the UART drain before touching the finisher device.
    {
        unsigned long drain = rd_cycle();
        while (rd_cycle() - drain < 10000000UL)
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
