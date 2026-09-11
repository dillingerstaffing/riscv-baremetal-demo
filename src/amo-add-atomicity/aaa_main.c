// aaa_main.c: amoadd.w read-modify-write semantics on a single hart.
//
// M-mode bare metal on the QEMU virt board. Two phases, each on its own
// 4-byte aligned cell:
//
//   Phase A: cell_a = BASE_A; 10000 x amoadd.w with increment 1. Every
//     iteration checks the returned value against the expected old value
//     (base + i). After the loop the cell must equal BASE_A + 10000, and
//     the sum of all returned old values must equal the closed-form
//     arithmetic-series sum, which catches any duplicated or skipped
//     return value even if the final total happened to land right. A
//     second, byte-wise reconstruction read confirms the final word.
//   Phase B: cell_b = BASE_B; 100 x amoadd.w with increment 7, same
//     per-iteration old-value check and sum invariant, final must equal
//     BASE_B + 700.
//
// A minimal M-mode trap handler (aaa_trap.S) counts traps and parks the
// hart on the first one, so a printed RESULT: PASS implies zero traps;
// the count is printed and checked as well.
//
// What this does NOT show: one hart issuing back-to-back amoadd.w cannot
// observe contention, so this verifies the instruction's single-hart
// read-modify-write contract (old value returned each time, exact final
// value, no lost update), not atomicity under multi-hart contention.

#include "../uart.h"

extern void aaa_trap_entry(void);

// Two 4-byte cells, 4-byte aligned (amoadd.w needs natural alignment).
static volatile unsigned int cell_a __attribute__((aligned(4)));
static volatile unsigned int cell_b __attribute__((aligned(4)));

// Trap save area for aaa_trap.S:
// [0]=trap count [1]=mcause [2]=mepc [3]=mtval
volatile unsigned long aaa_save[4];

#define N_A    10000
#define N_B    100
#define BASE_A 0x10000000U
#define BASE_B 0x20000000U
#define INCR_B 7U

static unsigned long checks = 0;
static unsigned long mismatches = 0;
static int fails = 0;

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        mismatches++;
        fails++;
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
    }
}

static unsigned long read_cycle(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, cycle" : "=r"(v));
    return v;
}

static unsigned long csr_read_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

// One amoadd.w: atomically adds incr to *addr and returns the OLD value.
// -march=rv64imac_zicsr provides the A extension, so the assembler
// accepts the mnemonic directly.
static unsigned int amoadd_w(volatile unsigned int *addr, unsigned int incr) {
    unsigned int old;
    __asm__ volatile("amoadd.w %0, %2, 0(%1)"
                     : "=r"(old)
                     : "r"(addr), "r"(incr)
                     : "memory");
    return old;
}

// Independent readback: assemble the 32-bit word byte by byte,
// little-endian, so the final-value confirmation does not reuse the
// same word-load path as the loop.
static unsigned int ref_lw4(unsigned long addr) {
    volatile unsigned char *p = (volatile unsigned char *)addr;
    return (unsigned int)p[0]
         | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16)
         | ((unsigned int)p[3] << 24);
}

// FNV-1a 64-bit over the two final cell values: a fingerprint of the run.
static unsigned long fnv1a_64(unsigned int a, unsigned int b) {
    unsigned long h = 1469598103934665603UL;
    unsigned int words[2];
    int i, j;
    words[0] = a;
    words[1] = b;
    for (i = 0; i < 2; i++)
        for (j = 0; j < 4; j++) {
            h ^= (unsigned long)((words[i] >> (8 * j)) & 0xff);
            h *= 1099511628211UL;
        }
    return h;
}

int main(void) {
    unsigned int i, old, expected, final_a, final_b, rb;
    unsigned long c0, c1, cyc_a, sum_a, sum_b, cksum;
    unsigned long exp_sum_a, exp_sum_b;
    unsigned long first_bad = 0;
    int bad_seen = 0;

    uart_init();
    uart_puts("amo-add-atomicity: amoadd.w read-modify-write semantics\n");
    uart_puts("hart mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    // Install the trap vector (direct mode) and arm mscratch.
    __asm__ volatile("csrw mtvec, %0" :: "r"(aaa_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(aaa_save));

    // Control: the cells are good RAM for plain aligned access.
    cell_a = 0xA5A5A5A5U;
    cell_b = 0x5A5A5A5AU;
    check(cell_a == 0xA5A5A5A5U && cell_b == 0x5A5A5A5AU,
          "control: plain aligned load/store roundtrip failed");
    uart_puts("control: aligned load/store roundtrip: ok\n");

    // Phase A: 10000 x amoadd.w +1 from BASE_A.
    cell_a = BASE_A;
    expected = BASE_A;
    sum_a = 0;
    c0 = read_cycle();
    for (i = 0; i < N_A; i++) {
        old = amoadd_w(&cell_a, 1);
        checks++;
        if (old != expected) {
            mismatches++;
            if (!bad_seen) {
                bad_seen = 1;
                first_bad = i;
            }
        }
        sum_a += old;
        expected++;
    }
    c1 = read_cycle();
    cyc_a = c1 - c0;
    if (bad_seen) {
        fails++;
        uart_puts("  FAIL: phase A old-value mismatch, first at iteration ");
        uart_put_dec(first_bad);
        uart_puts("\n");
    }
    // Closed-form check on the whole returned stream: the old values must
    // be exactly BASE_A, BASE_A+1, ..., BASE_A+N_A-1 in order.
    exp_sum_a = (unsigned long)N_A * (unsigned long)BASE_A
              + (unsigned long)N_A * (unsigned long)(N_A - 1) / 2;
    check(sum_a == exp_sum_a,
          "phase A sum of returned old values != arithmetic-series sum");
    final_a = cell_a;
    check(final_a == BASE_A + N_A, "phase A final != BASE_A + 10000");
    rb = ref_lw4((unsigned long)&cell_a);
    check(rb == BASE_A + N_A,
          "phase A byte-wise readback != BASE_A + 10000");
    uart_puts("phase A: final=");
    uart_put_hex(final_a);
    uart_puts(" expected=");
    uart_put_hex(BASE_A + N_A);
    uart_puts(" cycles=");
    uart_put_dec(cyc_a);
    uart_puts("\n");

    // Phase B: 100 x amoadd.w +7 from BASE_B.
    cell_b = BASE_B;
    expected = BASE_B;
    sum_b = 0;
    bad_seen = 0;
    for (i = 0; i < N_B; i++) {
        old = amoadd_w(&cell_b, INCR_B);
        checks++;
        if (old != expected) {
            mismatches++;
            if (!bad_seen) {
                bad_seen = 1;
                first_bad = i;
            }
        }
        sum_b += old;
        expected += INCR_B;
    }
    if (bad_seen) {
        fails++;
        uart_puts("  FAIL: phase B old-value mismatch, first at iteration ");
        uart_put_dec(first_bad);
        uart_puts("\n");
    }
    exp_sum_b = (unsigned long)N_B * (unsigned long)BASE_B
              + (unsigned long)INCR_B * (unsigned long)N_B
                * (unsigned long)(N_B - 1) / 2;
    check(sum_b == exp_sum_b,
          "phase B sum of returned old values != arithmetic-series sum");
    final_b = cell_b;
    check(final_b == BASE_B + (unsigned long)INCR_B * N_B,
          "phase B final != BASE_B + 7*100");
    rb = ref_lw4((unsigned long)&cell_b);
    check(rb == BASE_B + (unsigned long)INCR_B * N_B,
          "phase B byte-wise readback != BASE_B + 7*100");
    uart_puts("phase B: final=");
    uart_put_hex(final_b);
    uart_puts(" expected=");
    uart_put_hex(BASE_B + (unsigned long)INCR_B * N_B);
    uart_puts("\n");

    // Zero traps: the handler parks the hart on any trap, so reaching
    // this point means none fired; the counter is checked explicitly.
    check(aaa_save[0] == 0, "trap handler ran (trap count != 0)");
    uart_puts("traps=");
    uart_put_dec(aaa_save[0]);
    uart_puts("\n");

    cksum = fnv1a_64(final_a, final_b);
    uart_puts("checksum=");
    uart_put_hex(cksum);
    uart_puts(" (FNV-1a over the two final cell values)\n");

    uart_puts("summary: checks=");
    uart_put_dec(checks);
    uart_puts(" mismatches=");
    uart_put_dec(mismatches);
    uart_puts("\n");

    if (fails == 0)
        uart_puts("RESULT: PASS\n");
    else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
