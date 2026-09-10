// mce_main.c: mcounteren M-mode gating check (backlog item 135).
//
// Mechanism under test: the mcounteren CSR (bits CY, TM, IR) gates
// counter reads for S and U mode only. M-mode reads of cycle/time/
// instret are never gated, regardless of the mcounteren bits. The
// module, running in M-mode on the QEMU virt board:
//
//   1. Reads the boot value of mcounteren (baseline), and rdcycle
//      twice to establish a baseline that the counter advances.
//   2. Writes 0x7 (CY|TM|IR) to mcounteren and requires the
//      readback to be exactly 0x7. This proves the CSR is really
//      writable here; QEMU boots with mcounteren = 0x0, so a lone
//      0 write would be indistinguishable from an ignored write.
//   3. Writes mcounteren = 0 with csrw, reads it back, and requires
//      the readback to be 0x0 (the write took effect).
//   4. Reads rdcycle several times with mcounteren = 0 and requires
//      every sample to be strictly larger than the previous one:
//      M-mode rdcycle is not gated, so the counter must keep
//      advancing even though all enable bits are clear.
//   5. Restores mcounteren to the boot value, reads it back, and
//      requires the readback to equal the boot value.
//
// A failed check prints FAIL and flips the verdict; RESULT: PASS is
// printed only when every check held.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.
//
// No trap handler: everything is a plain M-mode CSR read/write, and
// writing 0 to mcounteren can never trap a M-mode hart, so there is
// no faulting state to recover from.

#include "../uart.h"

#define NSAMPLES 5

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

static unsigned long read_cycle(void) {
    unsigned long v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

static unsigned long read_mcounteren(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mcounteren" : "=r"(v));
    return v;
}

static void write_mcounteren(unsigned long v) {
    __asm__ volatile("csrw mcounteren, %0" :: "r"(v));
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
    unsigned long boot_mcounteren, after_write, after_restore;
    unsigned long b0, b1;
    unsigned long samples[NSAMPLES];
    unsigned long i;
    int advancing = 1;

    uart_init();
    uart_puts("mcounteren-mmode-gating: M-mode rdcycle vs mcounteren=0\n");

    // 1. Boot baselines: mcounteren, and rdcycle advancement at boot.
    boot_mcounteren = read_mcounteren();
    uart_puts("boot: mcounteren=");
    uart_put_hex(boot_mcounteren);
    uart_puts("\n");

    b0 = read_cycle();
    b1 = read_cycle();
    uart_puts("boot: rdcycle pair = ");
    uart_put_hex(b0);
    uart_puts(" -> ");
    uart_put_hex(b1);
    uart_puts(" (delta=");
    uart_put_dec(b1 - b0);
    uart_puts(")\n");
    check(b1 > b0, "baseline rdcycle not advancing");

    // 2. Writability probe: write 0x7 (CY|TM|IR), read back; the
    // readback must be exactly 0x7 so the later 0 write is proven
    // to take effect rather than be silently ignored (boot value
    // is already 0x0, so a lone 0 write would be ambiguous).
    write_mcounteren(0x7UL);
    after_write = read_mcounteren();
    uart_puts("probe: csrw mcounteren, 0x7; readback=");
    uart_put_hex(after_write);
    uart_puts("\n");
    check(after_write == 0x7UL, "mcounteren write of 0x7 did not read back as 0x7");

    // 3. Gate: write mcounteren = 0, read back.
    write_mcounteren(0);
    after_write = read_mcounteren();
    uart_puts("write: csrw mcounteren, 0; readback=");
    uart_put_hex(after_write);
    uart_puts("\n");
    check(after_write == 0, "mcounteren write of 0 did not read back as 0");

    // 4. rdcycle must keep advancing in M-mode with mcounteren = 0.
    for (i = 0; i < NSAMPLES; i++) {
        samples[i] = read_cycle();
    }
    uart_puts("rdcycle samples with mcounteren=0:");
    for (i = 0; i < NSAMPLES; i++) {
        uart_puts(" ");
        uart_put_hex(samples[i]);
    }
    uart_puts("\n");
    for (i = 1; i < NSAMPLES; i++) {
        if (samples[i] <= samples[i - 1]) {
            advancing = 0;
        }
    }
    check(advancing, "rdcycle did not advance with mcounteren=0");
    uart_puts("rdcycle deltas with mcounteren=0:");
    for (i = 1; i < NSAMPLES; i++) {
        uart_puts(" +");
        uart_put_dec(samples[i] - samples[i - 1]);
    }
    uart_puts("\n");

    // 5. Restore mcounteren to the boot value, read back.
    write_mcounteren(boot_mcounteren);
    after_restore = read_mcounteren();
    uart_puts("restore: csrw mcounteren, ");
    uart_put_hex(boot_mcounteren);
    uart_puts("; readback=");
    uart_put_hex(after_restore);
    uart_puts("\n");
    check(after_restore == boot_mcounteren, "mcounteren restore did not read back as boot value");

    uart_puts(fails == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;
    }
    for (;;) {
        __asm__ volatile("wfi");
    }
    return 0;
}
