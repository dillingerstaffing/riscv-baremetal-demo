// mcw_main.c: menvcfg.CBZE write/readback WARL probe (backlog item
// riscv menvcfg-cbze-write).
//
// Mechanism under test: menvcfg.CBZE (bits 7:6) is a WARL field that
// governs the cache-block-zero extension behavior; a write of each
// legal value 0..3 must read back exactly what was written (or the
// implementation-legalized subset), and the boot value must be
// restorable exactly. The experiment writes CBZE=1, 2, 3 and then 0
// with all other bits held at their boot values and requires each
// readback to equal the written value bit-for-bit. An all-ones WARL
// probe is written twice and the legalized readback is published;
// asserted there are only the properties this hart must honor: the
// CBZE field survives (it is implemented), bits set at boot do not
// vanish, and the two legalizations agree. Finally the boot value is
// written back and must read back exactly, leaving no residue.
//
// The whole experiment runs in M-mode on hart 0, where menvcfg is
// writable; QEMU boots the ELF straight into M-mode with -bios none,
// so no privilege drop is needed. The program never issues a
// trapping instruction, so the minimal trap handler (mcw_trap.S)
// records mcause/mepc/mtval and a trap count, then parks the hart;
// reaching the printed PASS implies the trap count is zero, and the
// program also prints and checks the count explicitly. On PASS the
// machine shuts down through the virt test-device finisher (QEMU
// exits 0); on FAIL the hart parks in a wfi loop without touching
// the finisher.

#include "../uart.h"

extern void mcw_trap_entry(void);

// Trap record, written by mcw_trap.S: [0] parked t1,
// [1] mcause, [2] mepc, [3] mtval, [4] trap count.
volatile unsigned long mcw_save[8];

#define MENVCFG_CBZE_MASK (0xC0UL) // bits 7:6

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// FNV-1a (64-bit) over the verdict-relevant values in a fixed order:
// the boot value, the four CBZE write/readback pairs, the two
// legalized all-ones readbacks, and the restore readback. No
// absolute addresses or live counters enter the checksum, so it is
// byte-identical across runs.
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
    unsigned long boot, base, w, rb, warl1, warl2, rst_rb, traps;
    int v;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("menvcfg.CBZE (bits 7:6) write/readback WARL probe\n");
    uart_puts("========================================\n\n");

    // Trap vector: any trap parks the hart (mcw_trap.S).
    __asm__ volatile("la t0, mcw_trap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");

    // 1. Boot menvcfg, before any write.
    boot = read_menvcfg();
    uart_puts("boot: menvcfg=");
    uart_put_hex(boot);
    uart_puts(" CBZE(bits7:6)=");
    uart_put_dec((boot >> 6) & 3UL);
    uart_puts("\n");
    cks_feed(boot);

    // 2-5. CBZE probes: for each value 1, 2, 3, then 0, write the
    // value into bits 7:6 while holding every other bit at its boot
    // value, then read back. Each readback must equal the written
    // value exactly: the field must take every legal encoding, and
    // no other bit may move.
    base = boot & ~MENVCFG_CBZE_MASK;
    for (v = 1; v <= 3; v++) {
        w = base | ((unsigned long)v << 6);
        write_menvcfg(w);
        rb = read_menvcfg();
        uart_puts("cbze=");
        uart_put_dec((unsigned long)v);
        uart_puts(": write=");
        uart_put_hex(w);
        uart_puts(" readback=");
        uart_put_hex(rb);
        uart_puts("\n");
        if (v == 1) {
            check(rb == w,
                  "CBZE=1 probe: readback != write (field did not "
                  "stick exactly, or other bits changed)");
        } else if (v == 2) {
            check(rb == w,
                  "CBZE=2 probe: readback != write (field did not "
                  "stick exactly, or other bits changed)");
        } else {
            check(rb == w,
                  "CBZE=3 probe: readback != write (field did not "
                  "stick exactly, or other bits changed)");
        }
        cks_feed(rb);
    }
    w = base;
    write_menvcfg(w);
    rb = read_menvcfg();
    uart_puts("cbze=0: write=");
    uart_put_hex(w);
    uart_puts(" readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == w,
          "CBZE=0 probe: readback != write (field did not clear "
          "exactly, or other bits changed)");
    cks_feed(rb);

    // 6. All-ones WARL probe: publish the legalized readback, then
    // write all-ones again and require the same readback. Asserted:
    // the CBZE field survives the write (it is implemented on this
    // hart), bits that were set at boot do not vanish, and the
    // legalization is stable across the two writes. Everything else
    // is reported, not asserted: menvcfg is WARL, so arbitrary
    // bits may not survive.
    write_menvcfg(~0UL);
    warl1 = read_menvcfg();
    write_menvcfg(~0UL);
    warl2 = read_menvcfg();
    uart_puts("warl: write all-ones; legalized readback (1st)=");
    uart_put_hex(warl1);
    uart_puts("\nwarl: write all-ones; legalized readback (2nd)=");
    uart_put_hex(warl2);
    uart_puts("\n");
    check((warl1 & MENVCFG_CBZE_MASK) == MENVCFG_CBZE_MASK,
          "WARL probe: CBZE field did not read back all-set on "
          "all-ones write");
    check((warl1 & boot) == boot,
          "WARL probe: bits set at boot vanished under all-ones write");
    check(warl2 == warl1,
          "WARL probe: all-ones legalization not stable across two writes");
    cks_feed(warl1);
    cks_feed(warl2);

    // 7. Restore: write the boot value back, read back. The
    // readback must equal the boot value bit-for-bit, so the WARL
    // probe left no residue.
    write_menvcfg(boot);
    rst_rb = read_menvcfg();
    uart_puts("restore: write boot=");
    uart_put_hex(boot);
    uart_puts(" readback=");
    uart_put_hex(rst_rb);
    uart_puts("\n");
    check(rst_rb == boot,
          "restore: menvcfg did not read back the boot value exactly");
    cks_feed(rst_rb);

    // 8. Trap count: the handler parks on the first trap, so
    // reaching here already implies zero, but require it
    // explicitly as well.
    traps = mcw_save[4];
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
