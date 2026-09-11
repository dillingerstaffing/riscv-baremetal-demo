// mxl_main.c: misa.MXL WARL legalization check.
//
// Mechanism under test: misa.MXL is WARL, and this hart runs 64-bit
// code. Writing MXL=1 (RV32) while preserving the extension bitmap
// must not change the hart's XLEN: the hart legalizes the write,
// the MXL field reads back as 2 (RV64), and the extension bits read
// back exactly as at boot. Unlike src/misa-readonly, which only
// read the register, this module probes WARL legalization on write.
//
// Sequence:
//   1. Read misa at boot as the baseline.
//   2. Write (boot & ~(3<<62)) | (1<<62): attempt MXL=1, preserving
//      every other bit of the boot value, including extensions.
//   3. Read back. Require the MXL field ((v>>62)&3) to still be 2,
//      and bits 25:0 to be bit-identical to the boot value.
//   4. Write the boot value back and require the restore readback
//      to equal the boot value exactly.
// A trap handler is installed (direct-mode mtvec, mscratch scratch
// area) but must never fire: writing misa is a legal M-mode CSR
// write, even with an illegal MXL combination (WARL legalization),
// so any trap means the hart was disturbed and fails the verdict.
//
// A failed check prints FAIL and flips the verdict; RESULT: PASS is
// printed only when every check held.
//
// The FNV-1a checksum covers the published verdict values (boot,
// written, readback, restored, trap count); it is printed so the
// three runs can be compared byte-for-byte.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.

#include "../uart.h"

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

#define MXL_MASK (3UL << 62)
#define EXT_MASK 0x3FFFFFFUL  // misa bits 25:0, the extension bitmap

extern void mxl_trap_entry(void);

// Trap scratch: recorded mcause, mepc, mtval, and trap count.
// BSS-cleared to zero by boot.S.
unsigned long mxl_regs[4];

static unsigned long read_misa(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, misa" : "=r"(v));
    return v;
}

static void write_misa(unsigned long v) {
    __asm__ volatile("csrw misa, %0" : : "r"(v));
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

// FNV-1a (64-bit) over the serialized values.
static unsigned long fnv_update(unsigned long h, unsigned long v) {
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (v >> (i * 8)) & 0xFFUL;
        h *= 0x100000001B3UL;
    }
    return h;
}

int main(void) {
    unsigned long mtvec;
    unsigned long boot, attempt, readback, restored;
    unsigned long checksum;

    uart_init();
    uart_puts("misa-mxl-warl: misa.MXL WARL legalization check\n");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the recording area.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mxl_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mxl_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    check((mtvec & ~3UL) == (unsigned long)mxl_trap_entry,
          "mtvec did not take the handler address");
    check((mtvec & 3UL) == 0, "mtvec not in direct mode");

    // 1. Baseline: read misa at boot.
    boot = read_misa();
    uart_puts("boot: misa=");
    uart_put_hex(boot);
    uart_puts("\n");

    // 2. Attempt the switch to 32-bit, preserving everything else.
    attempt = (boot & ~MXL_MASK) | (1UL << 62);
    write_misa(attempt);
    uart_puts("write: value=");
    uart_put_hex(attempt);
    uart_puts("\n");

    // 3. Read back. The hart must ignore the MXL change: the field
    // reads back 2 (RV64) and the extension bitmap is untouched.
    readback = read_misa();
    uart_puts("readback: misa=");
    uart_put_hex(readback);
    uart_puts("\n");
    check(((readback >> 62) & 3UL) == 2UL,
          "MXL field changed: the write altered the hart's XLEN encoding");
    check((readback & EXT_MASK) == (boot & EXT_MASK),
          "extension bits 25:0 differ from the boot value");

    // 4. Restore the boot value and confirm the readback is exact.
    write_misa(boot);
    restored = read_misa();
    uart_puts("restore: misa=");
    uart_put_hex(restored);
    uart_puts("\n");
    check(restored == boot,
          "restoring the boot misa value did not read back exactly");

    // The trap handler must never have fired.
    if (mxl_regs[3] != 0) {
        uart_puts("  FAIL: unexpected trap(s), count=");
        uart_put_dec(mxl_regs[3]);
        uart_puts(" mcause=");
        uart_put_hex(mxl_regs[0]);
        uart_puts(" mepc=");
        uart_put_hex(mxl_regs[1]);
        uart_puts(" mtval=");
        uart_put_hex(mxl_regs[2]);
        uart_puts("\n");
        fails++;
    }
    uart_puts("traps: count=");
    uart_put_dec(mxl_regs[3]);
    uart_puts("\n");

    // Checksum over the verdict-relevant published values, so the
    // three runs are comparable byte-for-byte.
    checksum = 0xCBF29CE484222325UL;
    checksum = fnv_update(checksum, boot);
    checksum = fnv_update(checksum, attempt);
    checksum = fnv_update(checksum, readback);
    checksum = fnv_update(checksum, restored);
    checksum = fnv_update(checksum, mxl_regs[3]);
    uart_puts("checksum=");
    uart_put_hex(checksum);
    uart_puts("\n");

    if (fails == 0) {
        uart_puts("RESULT: PASS\n");
        *VIRT_TEST_FINISHER = FINISHER_PASS;
        for (;;) { }
    }
    uart_puts("RESULT: FAIL\n");
    // Park the hart; the harness observes the timeout exit status.
    for (;;) {
        __asm__ volatile("wfi");
    }
}
