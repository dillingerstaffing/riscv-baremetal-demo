// lsi_main.c: intervening-store reservation-invalidation experiment.
//
// Single hart, M-mode, QEMU virt. Two experiments on two aligned
// 4-byte cells (cell_a, cell_b):
//
//   (a) lr.w on cell_a, then a plain sw to the SAME address, then
//       sc.w on cell_a. The store between the load-reserved and the
//       store-conditional may invalidate the reservation, so the
//       sc must report failure (rd != 0); the intervening sw must
//       be visible in memory and the failed sc must have stored
//       nothing.
//   (b) control: lr.w on cell_b immediately followed by sc.w on
//       cell_b. This pins down that the reservation mechanism
//       itself works in this environment, so the nonzero rd in (a)
//       cannot be explained as "sc always fails here".
//
// The three-instruction sequence in (a) is emitted as one volatile
// asm block, so the compiler cannot reorder or delete the
// intervening store; the disassembly check in PROOF.md confirms the
// emitted order is lr.w / sw / sc.w.
//
// A minimal M-mode trap handler (lsi_trap.S) records
// mcause/mepc/mtval into lsi_save and halts the hart; no trap is
// expected, and the program checks the handler's seen flag as part
// of the verdict, so an unexpected trap would fail the run instead
// of passing silently.

#include "../uart.h"

extern void lsi_trap_entry(void);

// Trap save area for lsi_trap.S:
// [0]=mcause [1]=mepc [2]=mtval [3]=seen flag
volatile unsigned long lsi_save[4];

static volatile unsigned int cell_a __attribute__((aligned(4)));
static volatile unsigned int cell_b __attribute__((aligned(4)));

#define A_INIT 0xAAAA0000U   // cell_a initial value
#define A_SW   0xBBBB1111U   // value written by the intervening sw
#define A_SC   0xCCCC2222U   // value the sc would have stored on success
#define B_INIT 0xDDDD3333U   // cell_b initial value
#define B_SC   0xEEEE4444U   // value the control sc stores

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

static unsigned long csr_read_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

// (a) lr.w, then a plain sw to the same address, then sc.w.
// Returns the sc result in rd.
static unsigned long lr_sw_sc(volatile unsigned int *addr,
                             unsigned int sw_val, unsigned int sc_val) {
    unsigned long rd;
    __asm__ volatile(
        "lr.w t1, 0(%1)\n\t"
        "sw %2, 0(%1)\n\t"
        "sc.w %0, %3, 0(%1)\n\t"
        : "=&r" (rd)
        : "r" (addr), "r" (sw_val), "r" (sc_val)
        : "t1", "memory");
    return rd;
}

// (b) control: lr.w immediately followed by sc.w on the same cell.
static unsigned long lr_sc(volatile unsigned int *addr, unsigned int sc_val) {
    unsigned long rd;
    __asm__ volatile(
        "lr.w t1, 0(%1)\n\t"
        "sc.w %0, %2, 0(%1)\n\t"
        : "=&r" (rd)
        : "r" (addr), "r" (sc_val)
        : "t1", "memory");
    return rd;
}

// FNV-1a 64-bit over the verdict-relevant values, printed in the run
// log and stamped into PROOF.md. Identical across runs proves the
// run's numbers are the same numbers, not a fresh fluke.
static unsigned long fnv1a(unsigned long a, unsigned long b,
                           unsigned long c, unsigned long d) {
    unsigned long h = 0xcbf29ce484222325UL;
    unsigned long vals[4] = {a, b, c, d};
    unsigned int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 8; j++) {
            h ^= (vals[i] >> (8 * j)) & 0xFFUL;
            h *= 0x100000001b3UL;
        }
    }
    return h;
}

int main(void) {
    unsigned int before_a, before_b, after_a, after_b;
    unsigned long rd_a, rd_b, csum;

    uart_init();
    uart_puts("lrsc-store-invalidate: intervening-store reservation test\n");
    uart_puts("hart mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    // Install the trap vector (direct mode) and arm mscratch. No trap
    // is expected; the handler halts the hart, so a trap would end
    // the run without printing RESULT: PASS.
    __asm__ volatile("csrw mtvec, %0" :: "r"(lsi_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(lsi_save));
    uart_puts("trap vector installed (halts the hart on any trap)\n");

    cell_a = A_INIT;
    cell_b = B_INIT;

    // Experiment (a): lr.w, intervening sw, sc.w on cell_a.
    before_a = cell_a;
    rd_a = lr_sw_sc(&cell_a, A_SW, A_SC);
    after_a = cell_a;
    uart_puts("test(a) lr.w; sw; sc.w on A (same address):\n");
    uart_puts("  before_a=");
    uart_put_hex(before_a);
    uart_puts(" rd_a=");
    uart_put_dec(rd_a);
    uart_puts(" after_a=");
    uart_put_hex(after_a);
    uart_puts("\n");
    check(rd_a != 0,
          "test(a): sc after an intervening store reported success");
    check(after_a == A_SW,
          "test(a): intervening sw not visible in memory");
    check(after_a != A_SC,
          "test(a): failed sc wrote its value anyway");

    // Experiment (b): control, lr.w then sc.w on cell_b.
    before_b = cell_b;
    rd_b = lr_sc(&cell_b, B_SC);
    after_b = cell_b;
    uart_puts("test(b) control: lr.w; sc.w on B (no intervening store):\n");
    uart_puts("  before_b=");
    uart_put_hex(before_b);
    uart_puts(" rd_b=");
    uart_put_dec(rd_b);
    uart_puts(" after_b=");
    uart_put_hex(after_b);
    uart_puts("\n");
    check(rd_b == 0,
          "test(b): control pair did not report success");
    check(after_b == B_SC,
          "test(b): control store not visible in memory");

    // Trap accounting: zero traps must have fired across all tests.
    uart_puts("trap seen-flag=");
    uart_put_dec(lsi_save[3]);
    uart_puts("\n");
    check(lsi_save[3] == 0, "unexpected trap fired during the run");
    if (lsi_save[3] != 0) {
        uart_puts("  trap mcause=");
        uart_put_hex(lsi_save[0]);
        uart_puts(" mepc=");
        uart_put_hex(lsi_save[1]);
        uart_puts(" mtval=");
        uart_put_hex(lsi_save[2]);
        uart_puts("\n");
    }

    csum = fnv1a(rd_a, (unsigned long)after_a, rd_b,
                 (unsigned long)after_b);
    uart_puts("checksum=");
    uart_put_hex(csum);
    uart_puts("\n");
    uart_puts("checks=");
    uart_put_dec((unsigned long)checks);
    uart_puts(" fails=");
    uart_put_dec((unsigned long)fails);
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
