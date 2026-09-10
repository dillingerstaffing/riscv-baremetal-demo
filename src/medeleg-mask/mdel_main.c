// mdel_main.c: medeleg/mideleg writable-mask measurement
// (backlog item 84).
//
// Exactly one mechanism is under test: the set of writable bits in
// the M-mode trap-delegation CSRs medeleg and mideleg. The program:
//
//   1. Records the boot-time values of medeleg and mideleg.
//   2. Triggers one M-mode ecall and records mcause/mepc/mtval
//      (baseline trap behavior).
//   3. Writes all-ones to medeleg, reads back the writable mask;
//      writes all-ones twice and requires both readbacks to agree
//      (stability, no hard-coded expectation). Same for mideleg.
//   4. Restores both CSRs to their recorded boot values and reads
//      them back to confirm.
//   5. Triggers a second M-mode ecall and requires mcause, mepc and
//      mtval to match the baseline exactly.
//
// The only assertion the privileged specification guarantees a
// priori is that a CSR write followed by a read returns the
// written value filtered through the implementation's writable
// mask. The mask itself is implementation-defined, so the program
// reports it and the proof comes from stability across repeated
// write/read cycles and across runs, plus unchanged trap behavior
// after the restore.

#include "../uart.h"

static volatile unsigned long mdel_regs[8];  // trap scratch, mscratch points here

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long csr_read_medeleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, medeleg" : "=r"(v));
    return v;
}

static unsigned long csr_read_mideleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mideleg" : "=r"(v));
    return v;
}

static void csr_write_medeleg(unsigned long v) {
    __asm__ volatile("csrw medeleg, %0" : : "r"(v));
}

static void csr_write_mideleg(unsigned long v) {
    __asm__ volatile("csrw mideleg, %0" : : "r"(v));
}

extern void mdel_trap_entry(void);

// One synchronous trap from M-mode. The handler in mdel_trap.S
// records mcause/mepc/mtval in mdel_regs, skips the ecall (mepc += 4),
// and returns; this function returns only after the trap resolved.
__attribute__((noinline)) static void trap_once(void) {
    __asm__ volatile("ecall" ::: "memory");
}

static void report_trap(const char *tag, unsigned long n) {
    uart_puts(tag);
    uart_puts(": traps=");
    uart_put_dec(mdel_regs[0]);
    uart_puts(" mcause=");
    uart_put_hex(mdel_regs[2]);
    uart_puts(" mepc=");
    uart_put_hex(mdel_regs[3]);
    uart_puts(" mtval=");
    uart_put_hex(mdel_regs[4]);
    uart_puts("\n");
    check(mdel_regs[0] == n, "trap counter did not advance by exactly one");
}

int main(void) {
    unsigned long boot_medeleg, boot_mideleg;
    unsigned long md_a, md_b, mi_a, mi_b;
    unsigned long c1, e1, t1;
    unsigned long rd_medeleg, rd_mideleg;

    uart_init();
    uart_puts("medeleg-mask: medeleg/mideleg writable-bit measurement\n");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the scratch area.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mdel_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mdel_regs));
    {
        unsigned long tv;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        check((tv & ~3UL) == (unsigned long)mdel_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
    }

    // 1. Boot-time values.
    boot_medeleg = csr_read_medeleg();
    boot_mideleg = csr_read_mideleg();
    uart_puts("boot: medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts(" mideleg=");
    uart_put_hex(boot_mideleg);
    uart_puts("\n");

    // 2. Baseline trap: one M-mode ecall. Record what we see.
    trap_once();
    report_trap("trap1", 1);
    c1 = mdel_regs[2];
    e1 = mdel_regs[3];
    t1 = mdel_regs[4];

    // 3. Writable mask of medeleg: write all-ones twice, read back
    // each time. The two readbacks must agree; the value itself is
    // reported, not assumed.
    csr_write_medeleg(0xFFFFFFFFFFFFFFFFUL);
    md_a = csr_read_medeleg();
    csr_write_medeleg(0xFFFFFFFFFFFFFFFFUL);
    md_b = csr_read_medeleg();
    uart_puts("mask: medeleg-write=0xffffffffffffffff readback1=");
    uart_put_hex(md_a);
    uart_puts(" readback2=");
    uart_put_hex(md_b);
    uart_puts("\n");
    check(md_a == md_b, "medeleg readback not stable across two writes");

    // Same for mideleg.
    csr_write_mideleg(0xFFFFFFFFFFFFFFFFUL);
    mi_a = csr_read_mideleg();
    csr_write_mideleg(0xFFFFFFFFFFFFFFFFUL);
    mi_b = csr_read_mideleg();
    uart_puts("mask: mideleg-write=0xffffffffffffffff readback1=");
    uart_put_hex(mi_a);
    uart_puts(" readback2=");
    uart_put_hex(mi_b);
    uart_puts("\n");
    check(mi_a == mi_b, "mideleg readback not stable across two writes");

    // 4. Restore both CSRs to their boot values (zero on this
    // machine) and confirm by reading back.
    csr_write_medeleg(boot_medeleg);
    csr_write_mideleg(boot_mideleg);
    rd_medeleg = csr_read_medeleg();
    rd_mideleg = csr_read_mideleg();
    uart_puts("restore: medeleg=");
    uart_put_hex(rd_medeleg);
    uart_puts(" mideleg=");
    uart_put_hex(rd_mideleg);
    uart_puts("\n");
    check(rd_medeleg == boot_medeleg, "medeleg not restored to boot value");
    check(rd_mideleg == boot_mideleg, "mideleg not restored to boot value");

    // 5. Post-restore trap: the trap registers must match the
    // baseline exactly.
    trap_once();
    report_trap("trap2", 2);
    check(mdel_regs[2] == c1, "post-restore mcause differs from baseline");
    check(mdel_regs[3] == e1, "post-restore mepc differs from baseline");
    check(mdel_regs[4] == t1, "post-restore mtval differs from baseline");

    if (fails == 0) {
        uart_puts("RESULT: PASS\n");
    } else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
