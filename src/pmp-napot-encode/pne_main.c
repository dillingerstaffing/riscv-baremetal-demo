// pne_main.c: PMP NAPOT encode write/readback test
// (backlog item "riscv pmp-napot-encode").
//
// Exactly one mechanism is under test: the NAPOT encoding of a PMP
// address register. A naturally aligned power-of-two region is
// encoded as pmpaddr = (base >> 2) | ((size >> 3) - 1): the address
// above the region's granularity bits, plus a tail of trailing ones
// whose count names the size. The run writes three such patterns
// (4 KiB at 0x80002000, 64 KiB at 0x80400000, 1 MiB at 0x88000000)
// to pmpaddr0 and requires each to read back exactly as written,
// then programs pmpcfg0 entry 0 with A = NAPOT (bits 4:3 = 0b11)
// and requires the A field to read back as 3. pmpcfg0 is also
// tried at 0x1F (NAPOT + R + W + X, lock bit clear) to show the
// permission bits coexist with the address-matching field.
//
// M-mode only, no traps expected: every entry keeps L = 0, and
// unlocked PMP deny entries do not restrict M-mode at all, so the
// probe cannot fault. pmpaddr0, pmpaddr1, and pmpcfg0 are restored
// to their boot readbacks at the end, and the restore itself is
// checked.
//
// Sequence:
//   1. record boot pmpaddr0/pmpaddr1/pmpcfg0 readbacks and print them
//   2. for each NAPOT pattern: write to pmpaddr0, read back, require
//      equality, print the written-vs-readback table row
//   3. write pmpcfg0 = 0x18 (entry 0: A=NAPOT), read back, require
//      the entry-0 A field to be 3; write pmpcfg0 = 0x1F, read back,
//      require entry 0 to read 0x1F exactly
//   4. restore the three CSRs to boot values, require the restore
//      readbacks to match boot exactly
//   5. print the written-vs-readback table, the FNV-1a checksum over
//      the recorded values, and the verdict. On PASS the machine
//      shuts down via the virt test-device finisher so the QEMU
//      process exit code (0) reflects the verdict; on FAIL the hart
//      parks without touching the finisher.

#include "../uart.h"

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final RESULT line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

static unsigned long checks = 0;
static unsigned long fails = 0;

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long csr_read_pmpaddr0(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpaddr0" : "=r"(v));
    return v;
}

static void csr_write_pmpaddr0(unsigned long v) {
    __asm__ volatile("csrw pmpaddr0, %0" :: "r"(v));
}

static unsigned long csr_read_pmpaddr1(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpaddr1" : "=r"(v));
    return v;
}

static void csr_write_pmpaddr1(unsigned long v) {
    __asm__ volatile("csrw pmpaddr1, %0" :: "r"(v));
}

static unsigned long csr_read_pmpcfg0(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(v));
    return v;
}

static void csr_write_pmpcfg0(unsigned long v) {
    __asm__ volatile("csrw pmpcfg0, %0" :: "r"(v));
}

// NAPOT encodings: (base >> 2) | ((size >> 3) - 1).
//   4 KiB  @ 0x80002000: (0x80002000 >> 2) | 0x1ff   = 0x20000800 | 0x1ff
//   64 KiB @ 0x80400000: (0x80400000 >> 2) | 0x1fff  = 0x20100000 | 0x1fff
//   1 MiB  @ 0x88000000: (0x88000000 >> 2) | 0x1ffff = 0x22000000 | 0x1ffff
#define NAPOT_PAT_4K  0x200009ffUL
#define NAPOT_PAT_64K 0x20101fffUL
#define NAPOT_PAT_1M  0x2201ffffUL

#define PMPCFG0_A_NAPOT 0x18UL  // entry 0: A field (bits 4:3) = 0b11
#define PMPCFG0_NAPOT_RWX 0x1fUL  // entry 0: NAPOT + R + W + X, L clear

static void print_pair(const char *tag, unsigned long written,
                       unsigned long readback) {
    uart_puts(tag);
    uart_puts(": write=");
    uart_put_hex(written);
    uart_puts(" readback=");
    uart_put_hex(readback);
    uart_puts("\n");
}

// FNV-1a 64-bit over the verdict record. Deterministic across runs
// because the record holds no timing-dependent fields.
static unsigned long record_checksum(const unsigned long *vals, int n) {
    unsigned long h = 1469598103934665603UL;
    int i, j;
    for (i = 0; i < n; i++)
        for (j = 0; j < 8; j++) {
            h ^= (vals[i] >> (j * 8)) & 0xffUL;
            h *= 1099511628211UL;
        }
    return h;
}

int main(void) {
    static const unsigned long patterns[3] = { NAPOT_PAT_4K, NAPOT_PAT_64K,
                                               NAPOT_PAT_1M };
    static const char *sizes[3] = { "4K", "64K", "1M" };
    unsigned long boot_addr0, boot_addr1, boot_cfg0;
    unsigned long rb, cfg_rb, restore;
    unsigned long rec[12];
    int rec_n = 0;
    int i;

    uart_init();
    uart_puts("pmp-napot-encode: PMP NAPOT write/readback test\n");

    boot_addr0 = csr_read_pmpaddr0();
    boot_addr1 = csr_read_pmpaddr1();
    boot_cfg0 = csr_read_pmpcfg0();
    print_pair("boot pmpaddr0", 0, boot_addr0);
    print_pair("boot pmpaddr1", 0, boot_addr1);
    print_pair("boot pmpcfg0", 0, boot_cfg0);
    rec[rec_n++] = boot_addr0;
    rec[rec_n++] = boot_addr1;
    rec[rec_n++] = boot_cfg0;

    for (i = 0; i < 3; i++) {
        csr_write_pmpaddr0(patterns[i]);
        rb = csr_read_pmpaddr0();
        uart_puts("napot ");
        uart_puts(sizes[i]);
        print_pair("", patterns[i], rb);
        check(rb == patterns[i], "pmpaddr0 readback != written NAPOT pattern");
        rec[rec_n++] = patterns[i];
        rec[rec_n++] = rb;
    }

    csr_write_pmpcfg0(PMPCFG0_A_NAPOT);
    cfg_rb = csr_read_pmpcfg0() & 0xffUL;
    print_pair("pmpcfg0 entry0 A=NAPOT", PMPCFG0_A_NAPOT, cfg_rb);
    check((cfg_rb & 0x18UL) == 0x18UL, "pmpcfg0 entry 0 A field != NAPOT (3)");
    check(cfg_rb == PMPCFG0_A_NAPOT,
          "pmpcfg0 entry 0 readback != written 0x18");
    rec[rec_n++] = PMPCFG0_A_NAPOT;
    rec[rec_n++] = cfg_rb;

    csr_write_pmpcfg0(PMPCFG0_NAPOT_RWX);
    cfg_rb = csr_read_pmpcfg0() & 0xffUL;
    print_pair("pmpcfg0 entry0 NAPOT+RWX", PMPCFG0_NAPOT_RWX, cfg_rb);
    check(cfg_rb == PMPCFG0_NAPOT_RWX,
          "pmpcfg0 entry 0 readback != written 0x1f");
    rec[rec_n++] = PMPCFG0_NAPOT_RWX;
    rec[rec_n++] = cfg_rb;

    // Restore to boot values, exactly, and check the restore.
    csr_write_pmpaddr0(boot_addr0);
    csr_write_pmpaddr1(boot_addr1);
    csr_write_pmpcfg0(boot_cfg0);
    restore = csr_read_pmpaddr0();
    print_pair("restore pmpaddr0", boot_addr0, restore);
    check(restore == boot_addr0, "pmpaddr0 restore != boot value");
    restore = csr_read_pmpaddr1();
    print_pair("restore pmpaddr1", boot_addr1, restore);
    check(restore == boot_addr1, "pmpaddr1 restore != boot value");
    restore = csr_read_pmpcfg0();
    print_pair("restore pmpcfg0", boot_cfg0, restore);
    check(restore == boot_cfg0, "pmpcfg0 restore != boot value");
    rec[rec_n++] = restore;

    {
        unsigned long checksum = record_checksum(rec, rec_n);
        uart_puts("record checksum=");
        uart_put_hex(checksum);
        uart_puts("\n");
    }

    if (fails == 0) {
        uart_puts("RESULT: PASS (checks=");
        uart_put_dec(checks);
        uart_puts(")\n");
        while (!(*UART0_LSR & LSR_TEMT))
            ;
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)                              // unreachable; guards against
            __asm__ volatile("wfi");          // any fall-through printing
    }
    uart_puts("RESULT: FAIL (checks=");
    uart_put_dec(checks);
    uart_puts(" fails=");
    uart_put_dec(fails);
    uart_puts(")\n");
    while (!(*UART0_LSR & LSR_TEMT))
        ;
    for (;;)
        __asm__ volatile("wfi");
}
