// dvm_main.c: mtvec direct-mode vectoring check.
//
// Mechanism under test: with mtvec MODE=00 (Direct), every trap
// enters at the single BASE address, whatever the cause; mcause
// tells the traps apart. The program installs mtvec = BASE with
// MODE 00, reads it back, then fires two synchronous traps from
// two labeled sites: an illegal 32-bit instruction word (expects
// mcause = 2) and an M-mode ecall (expects mcause = 11). The
// handler in dvm_trap.S records, per trap, the address of the
// entry that actually ran (its own link-time address), mcause,
// and mepc, and bumps a per-cause counter.
//
// The verdict requires: mtvec readback equals the written BASE
// with MODE bits 00; both traps recorded the same landing address
// equal to BASE; the illegal trap carried mcause = 2 and
// mepc == the illegal site; the ecall carried mcause = 11 and
// mepc == the ecall site; each per-cause counter is 1; no
// unexpected trap fired.
//
// The trap-site addresses are taken with in-asm numeric local
// labels inside the faulting asm blocks: a plain C &&label at -O2
// is not pinned to the instruction it labels, while the assembler
// resolves its own label exactly.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop
// without touching the finisher; the bench harness runs QEMU
// under `timeout`, so a FAIL is observable as the timeout exit
// status (124) in addition to the RESULT: FAIL line.

#include "../uart.h"

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

extern void dvm_trap_entry(void);

// dvm_regs layout, shared with dvm_trap.S:
// [0..2] illegal trap: landing, mcause, mepc
// [3] illegal count, [4] ecall count
// [5] unexpected count, [6] unexpected mcause
// [7..9] ecall trap: landing, mcause, mepc
static volatile unsigned long dvm_regs[12];

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// FNV-1a 64-bit over the verdict-relevant published values,
// hashed word by word, little-endian byte order.
static unsigned long fnv1a_words(const unsigned long *w, unsigned long n) {
    unsigned long h = 0xcbf29ce484222325UL;
    unsigned long i, j;
    for (i = 0; i < n; i++) {
        unsigned long v = w[i];
        for (j = 0; j < 8; j++) {
            h ^= (v >> (j * 8)) & 0xffUL;
            h *= 0x100000001b3UL;
        }
    }
    return h;
}

int main(void) {
    unsigned long mtvec_written, mtvec_readback, base;
    unsigned long ill_site, ecall_site;
    unsigned long pub[9];
    unsigned long sum;

    uart_init();
    uart_puts("mtvec-direct-vectoring: direct-mode trap landing test\n");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the recording area. MODE=00 keeps the low two bits clear,
    // so the written value is the entry address itself.
    base = (unsigned long)dvm_trap_entry;
    __asm__ volatile("csrw mtvec, %0" : : "r"(base));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)dvm_regs));
    mtvec_written = base;
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec_readback));
    uart_puts("mtvec: written=");
    uart_put_hex(mtvec_written);
    uart_puts(" readback=");
    uart_put_hex(mtvec_readback);
    uart_puts("\n");
    check(mtvec_readback == mtvec_written,
          "mtvec readback differs from written value");
    check((mtvec_readback & 3UL) == 0,
          "mtvec MODE bits are not 00 (direct)");
    check((mtvec_readback & ~3UL) == base,
          "mtvec BASE is not the handler entry address");

    // Trap 1: illegal instruction. The site address is taken with an
    // in-asm local label so it is exactly the faulting instruction.
    __asm__ volatile(
        "la %0, 1f\n"
        "1:\n"
        "  .word 0x00000000\n"   // all-zero word: no valid RV64 encoding
        : "=r"(ill_site) : : "memory");

    // Trap 2: M-mode ecall.
    __asm__ volatile(
        "la %0, 2f\n"
        "2:\n"
        "  ecall\n"
        : "=r"(ecall_site) : : "memory");

    // Publish the per-trap recordings.
    uart_puts("illegal: count=");
    uart_put_dec(dvm_regs[3]);
    uart_puts(" mcause=");
    uart_put_hex(dvm_regs[1]);
    uart_puts(" mepc=");
    uart_put_hex(dvm_regs[2]);
    uart_puts(" landing=");
    uart_put_hex(dvm_regs[0]);
    uart_puts("\n");
    uart_puts("ecall: count=");
    uart_put_dec(dvm_regs[4]);
    uart_puts(" mcause=");
    uart_put_hex(dvm_regs[8]);
    uart_puts(" mepc=");
    uart_put_hex(dvm_regs[9]);
    uart_puts(" landing=");
    uart_put_hex(dvm_regs[7]);
    uart_puts("\n");
    uart_puts("sites: illegal=");
    uart_put_hex(ill_site);
    uart_puts(" ecall=");
    uart_put_hex(ecall_site);
    uart_puts("\n");
    uart_puts("unexpected: count=");
    uart_put_dec(dvm_regs[5]);
    uart_puts(" mcause=");
    uart_put_hex(dvm_regs[6]);
    uart_puts("\n");

    // Verdict: in Direct mode both traps must have entered at BASE,
    // each carrying its own cause, each with mepc at its own site.
    check(dvm_regs[3] == 1, "illegal-instruction trap count != 1");
    check(dvm_regs[4] == 1, "ecall trap count != 1");
    check(dvm_regs[5] == 0, "unexpected trap(s) fired");
    check(dvm_regs[1] == 2, "illegal trap mcause != 2");
    check(dvm_regs[8] == 11, "ecall trap mcause != 11");
    check(dvm_regs[2] == ill_site, "illegal trap mepc != illegal site");
    check(dvm_regs[9] == ecall_site, "ecall trap mepc != ecall site");
    check(dvm_regs[0] == base, "illegal trap did not land at BASE");
    check(dvm_regs[7] == base, "ecall trap did not land at BASE");
    check(dvm_regs[0] == dvm_regs[7],
          "the two traps landed at different addresses");

    // Checksum over the verdict-relevant published values.
    pub[0] = mtvec_readback;
    pub[1] = dvm_regs[0];
    pub[2] = dvm_regs[1];
    pub[3] = dvm_regs[2];
    pub[4] = dvm_regs[7];
    pub[5] = dvm_regs[8];
    pub[6] = dvm_regs[9];
    pub[7] = dvm_regs[3];
    pub[8] = dvm_regs[4];
    sum = fnv1a_words(pub, 9);
    uart_puts("checksum: fnv1a64=");
    uart_put_hex(sum);
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
