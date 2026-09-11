// msw_main.c: menvcfg.STCE set/clear write/readback (backlog item
// riscv menvcfg-stce-write).
//
// Mechanism under test: menvcfg.STCE (bit 63) is the M-mode switch
// that hands the Sstc timer to S-mode, so the bit must read back as
// written and must not drag other bits with it. The whole experiment
// runs in M-mode on hart 0, where menvcfg is writable; QEMU boots the
// ELF straight into M-mode with -bios none, so no privilege drop is
// needed. The program never issues a trapping instruction, so the
// minimal trap handler (msw_trap.S) records mcause/mepc/mtval and a
// trap count, then parks the hart; reaching the printed PASS implies
// the trap count is zero, and the program also prints and checks the
// count explicitly. On PASS the machine shuts down through the virt
// test-device finisher (QEMU exits 0); on FAIL the hart parks in a
// wfi loop without touching the finisher.
//
// menvcfg is WARL, so only one thing is asserted about arbitrary
// bits: the STCE bit itself must read back exactly as written, and
// the set/clear probes must not disturb any other bit relative to
// what was written. The all-ones probe only reports the legalized
// readback honestly; the asserted properties there are that STCE
// survives the write (it is implemented on this hart), that bits
// already set at boot do not vanish, and that two all-ones writes
// legalize to the same value (stable legalization).

#include "../uart.h"

extern void msw_trap_entry(void);

// Trap record, written by msw_trap.S: [0] parked t1,
// [1] mcause, [2] mepc, [3] mtval, [4] trap count.
volatile unsigned long msw_save[8];

#define MENVCFG_STCE (1UL << 63)

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// FNV-1a (64-bit) over the verdict-relevant values in a fixed order:
// the boot value, the three write/readback pairs, and the two
// legalized all-ones readbacks. No absolute addresses or live
// counters enter the checksum, so it is byte-identical across runs.
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

static unsigned long read_menvcfg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, menvcfg" : "=r"(v));
    return v;
}

static void write_menvcfg(unsigned long v) {
    __asm__ volatile("csrw menvcfg, %0" :: "r"(v) : "memory");
}

int main(void) {
    unsigned long boot, set_w, set_rb, clr_w, clr_rb, warl1, warl2;
    unsigned long rst_rb, traps;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("menvcfg.STCE write/readback: set, clear, WARL legalize\n");
    uart_puts("========================================\n\n");

    // Trap vector: any trap parks the hart (msw_trap.S).
    __asm__ volatile("la t0, msw_trap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");

    // 1. Boot menvcfg, before any write.
    boot = read_menvcfg();
    uart_puts("boot: menvcfg=");
    uart_put_hex(boot);
    uart_puts(" STCE(bit63)=");
    uart_put_dec((boot >> 63) & 1UL);
    uart_puts("\n");
    cks_feed(boot);

    // 2. Set probe: write boot|STCE, read back. The readback must
    // carry bit 63 and no other bit may change relative to what
    // was written, proving the write took effect exactly.
    set_w = boot | MENVCFG_STCE;
    write_menvcfg(set_w);
    set_rb = read_menvcfg();
    uart_puts("set: write boot|STCE=");
    uart_put_hex(set_w);
    uart_puts(" readback=");
    uart_put_hex(set_rb);
    uart_puts("\n");
    check(set_rb == set_w,
          "STCE set probe: readback != write (bit did not stick "
          "exactly, or other bits changed)");
    cks_feed(set_rb);

    // 3. Clear probe: write boot with STCE clear, read back. The
    // readback must equal the written value exactly.
    clr_w = boot & ~MENVCFG_STCE;
    write_menvcfg(clr_w);
    clr_rb = read_menvcfg();
    uart_puts("clear: write boot&~STCE=");
    uart_put_hex(clr_w);
    uart_puts(" readback=");
    uart_put_hex(clr_rb);
    uart_puts("\n");
    check(clr_rb == clr_w,
          "STCE clear probe: readback != write (bit did not clear "
          "exactly, or other bits changed)");
    cks_feed(clr_rb);

    // 4. All-ones WARL probe: publish the legalized readback, then
    // write all-ones again and require the same readback. Asserted:
    // STCE survives (the bit is implemented on this hart), bits
    // that were set at boot do not vanish, and the legalization is
    // stable across the two writes. Everything else is reported,
    // not asserted: menvcfg is WARL, so arbitrary bits may not
    // survive.
    write_menvcfg(~0UL);
    warl1 = read_menvcfg();
    write_menvcfg(~0UL);
    warl2 = read_menvcfg();
    uart_puts("warl: write all-ones; legalized readback (1st)=");
    uart_put_hex(warl1);
    uart_puts("\nwarl: write all-ones; legalized readback (2nd)=");
    uart_put_hex(warl2);
    uart_puts("\n");
    check((warl1 & MENVCFG_STCE) != 0,
          "WARL probe: STCE bit did not read back set on all-ones write");
    check((warl1 & boot) == boot,
          "WARL probe: bits set at boot vanished under all-ones write");
    check(warl2 == warl1,
          "WARL probe: all-ones legalization not stable across two writes");
    cks_feed(warl1);
    cks_feed(warl2);

    // 5. Restore: write the boot value back, read back. The
    // readback must equal the boot value, so the WARL probe left
    // no residue.
    write_menvcfg(boot);
    rst_rb = read_menvcfg();
    uart_puts("restore: write boot=");
    uart_put_hex(boot);
    uart_puts(" readback=");
    uart_put_hex(rst_rb);
    uart_puts("\n");
    check(rst_rb == boot,
          "restore: menvcfg did not read back the boot value");
    cks_feed(rst_rb);

    // 6. Trap count: the handler parks on the first trap, so
    // reaching here already implies zero, but require it
    // explicitly as well.
    traps = msw_save[4];
    uart_puts("traps recorded by the handler: ");
    uart_put_dec(traps);
    uart_puts("\n");
    check(traps == 0, "unexpected trap fired during the run");

    uart_puts("\nchecksum (FNV-1a over the write/readback pairs) = ");
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
