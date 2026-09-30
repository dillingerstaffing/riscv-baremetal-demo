// mhr_main.c: mhartid read-only invariant check.
//
// Mechanism under test: the mhartid CSR is read-only and holds this
// hart's integer ID. The program:
//
//   1. Reads mhartid at boot (expect 0 on the QEMU virt boot hart;
//      the value is reported, and the verdict is readback == boot,
//      whatever the value is).
//   2. Installs a counting M-mode trap handler that records
//      mcause/mepc/mtval for up to two traps and resumes past the
//      faulting instruction.
//   3. csrw all-ones to mhartid. Per the RISC-V privileged spec, a
//      write to a read-only CSR raises an illegal-instruction
//      exception, so this write is expected to trap (mcause=0x2)
//      rather than commit; the handler records the trap and skips
//      the faulting csrw. What the machine does is measured, not
//      assumed: the invariant under test is only that the readback
//      never changes.
//   4. csrw zero to mhartid, same expectation.
//   5. Reads back after each write and requires the readback to be
//      bit-identical to the boot value.
//
// A failed check prints FAIL and flips the verdict; RESULT: PASS is
// printed only when every check held.
//
// Exit discipline mirrors the sibling misa-readonly module: on PASS
// the module writes the virt test-device finisher word 0x5555 at
// 0x100000, which shuts the machine down and QEMU exits 0. On FAIL
// it parks the hart in a wfi loop without touching the finisher;
// the bench harness runs QEMU under `timeout`, so a FAIL is
// observable as the timeout exit status (124) in addition to the
// RESULT: FAIL line.

#include "../uart.h"

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

extern void mhr_trap_entry(void);

// Trap scratch: [0] trap count; then two 3-word records
// (mcause, mepc-at-entry, mtval). BSS-cleared to zero by boot.S.
// Volatile: written by the trap handler in another translation unit.
volatile unsigned long mhr_regs[7];

static unsigned long read_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

static void write_mhartid(unsigned long v) {
    __asm__ volatile("csrw mhartid, %0" : : "r"(v));
}

static int fails = 0;
static int checks = 0;

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// FNV-1a 64-bit feed: one unsigned long, little-endian bytes.
static unsigned long fnv1a_feed(unsigned long h, unsigned long v) {
    int j;
    for (j = 0; j < 8; j++) {
        h ^= (v >> (8 * j)) & 0xffUL;
        h *= 1099511628211UL;
    }
    return h;
}

int main(void) {
    unsigned long mtvec;
    unsigned long boot, rb1, rb2;
    unsigned long count, i, nrec;
    unsigned long cksum;
    int ok;

    uart_init();
    uart_puts("mhartid-readonly: mhartid read-only invariant check\n");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the recording area.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mhr_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mhr_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    check((mtvec & ~3UL) == (unsigned long)mhr_trap_entry,
          "mtvec did not take the handler address");
    check((mtvec & 3UL) == 0, "mtvec not in direct mode");

    // 1. Baseline: read mhartid at boot.
    boot = read_mhartid();
    uart_puts("boot: mhartid=");
    uart_put_hex(boot);
    uart_puts("\n");
    check(boot == 0, "boot mhartid is not 0 on the QEMU virt boot hart");

    // 2. Attempt csrw all-ones to mhartid. The write is expected to
    // trap (illegal instruction, mcause=0x2) per the privileged
    // spec; the handler skips the faulting instruction and the run
    // continues. Whether it traps or is silently ignored is measured
    // below, not assumed here.
    write_mhartid(0xFFFFFFFFFFFFFFFFUL);
    rb1 = read_mhartid();
    uart_puts("write: value=0xffffffffffffffff readback=");
    uart_put_hex(rb1);
    uart_puts("\n");
    check(rb1 == boot, "mhartid changed after csrw all-ones");

    // 3. Attempt csrw zero to mhartid.
    write_mhartid(0UL);
    rb2 = read_mhartid();
    uart_puts("write: value=0x0 readback=");
    uart_put_hex(rb2);
    uart_puts("\n");
    check(rb2 == boot, "mhartid changed after csrw zero");

    // 4. Trap records: the two writes either both trapped (the
    // spec's behavior for a write to a read-only CSR) or neither
    // did (silently ignored). Anything else, or a trap with a cause
    // other than illegal instruction, fails the run.
    count = mhr_regs[0];
    uart_puts("traps: count=");
    uart_put_dec(count);
    uart_puts("\n");
    check(count == 0 || count == 2,
          "trap count is neither 0 nor 2: the two writes behaved differently");
    nrec = count < 2 ? count : 2;
    for (i = 0; i < nrec; i++) {
        uart_puts("trap[");
        uart_put_dec(i);
        uart_puts("]: mcause=");
        uart_put_hex(mhr_regs[1 + 3 * i]);
        uart_puts(" mepc=");
        uart_put_hex(mhr_regs[2 + 3 * i]);
        uart_puts(" mtval=");
        uart_put_hex(mhr_regs[3 + 3 * i]);
        uart_puts("\n");
    }
    ok = 1;
    for (i = 0; i < nrec; i++)
        if (mhr_regs[1 + 3 * i] != 2)
            ok = 0;
    check(ok, "a trap fired with mcause != 2 (illegal instruction)");

    // Fingerprint of everything measured above: the readback triple,
    // the trap count, and every recorded trap record.
    cksum = 1469598103934665603UL;
    cksum = fnv1a_feed(cksum, boot);
    cksum = fnv1a_feed(cksum, rb1);
    cksum = fnv1a_feed(cksum, rb2);
    cksum = fnv1a_feed(cksum, count);
    for (i = 0; i < nrec; i++) {
        cksum = fnv1a_feed(cksum, mhr_regs[1 + 3 * i]);
        cksum = fnv1a_feed(cksum, mhr_regs[2 + 3 * i]);
        cksum = fnv1a_feed(cksum, mhr_regs[3 + 3 * i]);
    }
    uart_puts("checksum=");
    uart_put_hex(cksum);
    uart_puts(" (FNV-1a over readbacks and trap records)\n");

    uart_puts("summary: checks=");
    uart_put_dec((unsigned long)checks);
    uart_puts(" mismatches=");
    uart_put_dec((unsigned long)fails);
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
