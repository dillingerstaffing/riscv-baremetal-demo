// mscratch_main.c: mscratch csrrw atomic-swap round-trip test
// (backlog item 173: riscv mscratch-csrrw-roundtrip).
//
// The Zicsr csrrw instruction atomically swaps a general-purpose
// register with a CSR: the destination register receives the CSR's old
// value and the CSR receives the source register's value. This module
// checks both sides of that swap on mscratch, then restores the boot
// value and verifies the restoration, all in M-mode on QEMU's virt
// machine.
//
// Test sequence (each csrrw operand is a single inline-asm instruction
// so the tested instruction is exactly csrrw against mscratch):
//   1. Read mscratch before touching it: the boot value. boot.S never
//      writes mscratch, so this is QEMU's reset value (measured below).
//   2. csrrw rd, mscratch, SENTINEL. Check rd == boot value (the read
//      side) and csrrs x0 readback == SENTINEL (the write side).
//   3. csrrw rd2, mscratch, boot_value. Check rd2 == SENTINEL and the
//      final csrr readback == boot value (the restore).
//   4. Check the trap counter is still 0 (the module spec is no traps).

#include "../uart.h"

extern void msc_trap_entry(void);

// Trap counter, incremented only by msc_trap.S.
volatile unsigned long msc_trapcount;

#define SENTINEL 0xA5A55A5A5A5A5A5UL

// FNV-1a 64-bit over the verdict-relevant values, so the three runs can
// be compared for byte identity by one checksum line.
static unsigned long fnv1a(unsigned long h, unsigned long v) {
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xFFUL;
        h *= 0x100000001B3UL;
    }
    return h;
}

static unsigned long csr_read_mscratch(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mscratch" : "=r"(v));
    return v;
}

// Atomic swap: rd <- old mscratch, mscratch <- newval. Returns rd.
static unsigned long csr_swap_mscratch(unsigned long newval) {
    unsigned long old;
    __asm__ volatile("csrrw %0, mscratch, %1" : "=r"(old) : "r"(newval));
    return old;
}

static unsigned long nchecks = 0;
static unsigned long fails = 0;

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
    unsigned long boot_val, rd, readback, rd2, restored, traps;
    unsigned long csum;

    uart_init();
    uart_puts("mscratch-csrrw: mscratch csrrw atomic-swap round-trip test\n");

    // Install the trap vector (direct mode) before anything else; the
    // counter is in BSS and therefore 0 after boot.S clears it.
    __asm__ volatile("csrw mtvec, %0" :: "r"(msc_trap_entry));

    // Step 1: the boot mscratch value, read before any write to mscratch.
    // boot.S does not reference mscratch, so this is QEMU's reset value.
    boot_val = csr_read_mscratch();
    uart_puts("boot:   mscratch=");
    uart_put_hex(boot_val);
    uart_puts(" (read before any write; boot.S does not touch mscratch)\n");

    // Step 2: atomic swap of the nonzero sentinel in; verify both sides.
    rd = csr_swap_mscratch(SENTINEL);
    readback = csr_read_mscratch();  // csrrs with x0: pure read
    uart_puts("swap:   csrrw rd,mscratch,SENTINEL  rd(old)=");
    uart_put_hex(rd);
    uart_puts(" mscratch(readback)=");
    uart_put_hex(readback);
    uart_puts("\n");
    check(rd == boot_val, "swap: rd != boot mscratch value (read side)");
    check(readback == SENTINEL,
          "swap: mscratch != SENTINEL after csrrw (write side)");

    // Step 3: restore the boot value exactly; verify both sides.
    rd2 = csr_swap_mscratch(boot_val);
    restored = csr_read_mscratch();
    uart_puts("restore: csrrw rd,mscratch,boot  rd(old)=");
    uart_put_hex(rd2);
    uart_puts(" mscratch(readback)=");
    uart_put_hex(restored);
    uart_puts("\n");
    check(rd2 == SENTINEL, "restore: rd != SENTINEL (read side)");
    check(restored == boot_val, "restore: mscratch != boot value");

    // Step 4: the module spec is zero traps.
    traps = msc_trapcount;
    uart_puts("traps:  ");
    uart_put_dec(traps);
    uart_puts(" (handler parks the hart, so a trap would end the run here)\n");
    check(traps == 0, "trap counter != 0");

    // Checksum over every verdict-relevant value, so the three runs can
    // be compared for identity by one line.
    csum = 0xCBF29CE484222325UL;
    csum = fnv1a(csum, boot_val);
    csum = fnv1a(csum, rd);
    csum = fnv1a(csum, readback);
    csum = fnv1a(csum, rd2);
    csum = fnv1a(csum, restored);
    csum = fnv1a(csum, traps);
    uart_puts("checksum=");
    uart_put_hex(csum);
    uart_puts(" (FNV-1a over boot, swap rd, swap readback, restore rd,\n");
    uart_puts("  restore readback, traps)\n");

    uart_puts("checks=");
    uart_put_dec(nchecks);
    uart_puts(" fails=");
    uart_put_dec(fails);
    uart_puts("\n");
    if (fails == 0)
        uart_puts("RESULT: PASS (swap both sides, restore exact, 0 traps)\n");
    else
        uart_puts("RESULT: FAIL\n");
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
