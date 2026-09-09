// fs_main.c: mstatus.FS field write/readback measurement
// (backlog item 80).
//
// Mechanism under test: the FS field (bits 14:13) of mstatus. The
// module captures a baseline mstatus in M-mode right after boot,
// then writes the FS field through all four values (0..3), reading
// back the full mstatus after each write. It reports, for each
// written value, the full readback word and the FS field the
// hardware reports, and it checks that no bit outside the FS field
// changed relative to the baseline on any write.
//
// What the emulator actually does (QEMU 8.2.2 source,
// target/riscv/csr.c, write_mstatus): the FS bits written are kept,
// and bit 63 (SD) is recomputed from the FS field: SD = 1 exactly
// when FS == 3. So the verdict checks are (1) every non-FS bit other
// than SD is identical to the baseline on every readback, (2) the
// FS field reads back exactly the written value on all 8 writes,
// (3) SD reads 1 exactly when FS was written as 3, and (4) the FS
// readback is a consistent function of the written value across
// both iterations. These are the measured hardware behaviors; the
// module does not assume them, it checks them, and the printed
// table is the evidence.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop
// without touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.
//
// No trap handler: everything is a plain M-mode CSR read/write.

#include "../uart.h"

#define FS_MASK  (0x3UL << 13)   // mstatus bits 14:13
#define SD_BIT   (1UL << 63)    // mstatus bit 63, summary of FS/XS/VS
#define NVALS    4               // FS values 0..3
#define NITERS   2               // full pass over 0..3, twice, for consistency

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

static unsigned long read_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static void write_mstatus(unsigned long v) {
    __asm__ volatile("csrw mstatus, %0" :: "r"(v));
}

static unsigned long read_mtime(void) {
    return *(volatile unsigned long *)0x0200bff8UL;
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
    unsigned long baseline;
    unsigned long fs_seen[NITERS][NVALS];   // FS field bits per write
    unsigned long v, iter;
    unsigned long leak = 0;                // unexpected bit changed somewhere
    int consistent = 1;                    // same write -> same FS, both iters
    int fs_exact = 1;                      // FS readback == written, all writes

    uart_init();
    uart_puts("fs-check: mstatus.FS field write/readback measurement\n");

    baseline = read_mstatus();
    uart_puts("baseline mstatus: ");
    uart_put_hex(baseline);
    uart_puts(" (FS bits read as ");
    uart_put_dec((baseline & FS_MASK) >> 13);
    uart_puts(")\n");

    for (iter = 0; iter < NITERS; iter++) {
        uart_puts("iteration ");
        uart_put_dec(iter);
        uart_puts(":\n");
        for (v = 0; v < NVALS; v++) {
            unsigned long w = (baseline & ~FS_MASK) | (v << 13);
            unsigned long r;
            write_mstatus(w);
            r = read_mstatus();
            fs_seen[iter][v] = (r & FS_MASK) >> 13;

            uart_puts("  wrote FS=");
            uart_put_dec(v);
            uart_puts(" readback=");
            uart_put_hex(r);
            uart_puts(" FS_readback=");
            uart_put_dec(fs_seen[iter][v]);
            uart_puts(" SD=");
            uart_put_dec((r >> 63) & 1);
            if (fs_seen[iter][v] != v)
                uart_puts(" FS_MISMATCH");
            if ((r & ~(FS_MASK | SD_BIT)) != (baseline & ~(FS_MASK | SD_BIT))) {
                uart_puts(" OTHER_BITS_CHANGED");
                leak = 1;
            }
            if (((r >> 63) & 1) != (v == 3 ? 1UL : 0UL)) {
                uart_puts(" SD_UNEXPECTED");
                leak = 1;
            }
            uart_puts("\n");
        }
    }

    // Restore the baseline FS field before the final checks print.
    write_mstatus(baseline);

    for (v = 0; v < NVALS; v++) {
        if (fs_seen[0][v] != fs_seen[1][v])
            consistent = 0;
        if (fs_seen[0][v] != v || fs_seen[1][v] != v)
            fs_exact = 0;
    }

    uart_puts("FS readback == written value on all writes: ");
    uart_puts(fs_exact ? "yes" : "no");
    uart_puts("\n");
    uart_puts("non-FS non-SD bits unchanged across all writes: ");
    uart_puts(leak == 0 ? "yes" : "no");
    uart_puts("\n");
    uart_puts("FS readback consistent across iterations: ");
    uart_puts(consistent ? "yes" : "no");
    uart_puts("\n");

    check(fs_exact, "FS field did not read back the written value");
    check(leak == 0, "a non-FS non-SD mstatus bit changed during an FS write");
    check(consistent, "same FS write read back differently between iterations");

    uart_puts("RESULT: ");
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
