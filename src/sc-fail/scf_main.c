// scf_main.c: store-conditional-without-reservation experiment.
//
// Single hart, M-mode, QEMU virt. Three experiments on two aligned
// 4-byte cells (cell_a, cell_b):
//
//   (a) sc.w issued with NO preceding lr.w: the rd result must be
//       nonzero (reservation failure), no trap may fire, and the
//       target word must read back unchanged (read before and after).
//   (b) lr.w on cell_a followed by sc.w on cell_b (a different
//       address): the rd result must be nonzero, no trap may fire,
//       and both words must read back unchanged (read both before
//       and after).
//   (c) control: lr.w on cell_a followed by sc.w on cell_a must
//       report success (rd == 0) and the stored value must read
//       back. This control pins down that the reservation mechanism
//       itself works in this environment, so a nonzero rd in (a)
//       and (b) cannot be explained as "sc always fails here".
//
// A minimal M-mode trap handler (scf_trap.S) records
// mcause/mepc/mtval into scf_save and halts the hart; no trap is
// expected, and the program checks the handler's seen flag as part
// of the verdict, so an unexpected trap would fail the run instead
// of passing silently.

#include "../uart.h"

extern void scf_trap_entry(void);

// Trap save area for scf_trap.S:
// [0]=mcause [1]=mepc [2]=mtval [3]=seen flag
volatile unsigned long scf_save[4];

static volatile unsigned int cell_a __attribute__((aligned(4)));
static volatile unsigned int cell_b __attribute__((aligned(4)));

static int fails = 0;

static void check(int cond, const char *msg) {
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

// (a) sc.w with no preceding lr.w. Returns the sc result in rd.
static unsigned long sc_no_lr(volatile unsigned int *addr, unsigned int val) {
    unsigned long rd;
    __asm__ volatile(
        "sc.w %0, %2, 0(%1)\n\t"
        : "=&r" (rd)
        : "r" (addr), "r" (val)
        : "memory");
    return rd;
}

// (b) lr.w on addr_lr, sc.w on a different address addr_sc.
static unsigned long lr_then_sc_other(volatile unsigned int *addr_lr,
                                      volatile unsigned int *addr_sc,
                                      unsigned int val) {
    unsigned long rd;
    __asm__ volatile(
        "lr.w t1, 0(%1)\n\t"
        "sc.w %0, %3, 0(%2)\n\t"
        : "=&r" (rd)
        : "r" (addr_lr), "r" (addr_sc), "r" (val)
        : "t1", "memory");
    return rd;
}

// (c) control: lr.w on addr immediately followed by sc.w on addr.
static unsigned long lr_then_sc_same(volatile unsigned int *addr,
                                     unsigned int val) {
    unsigned long rd;
    __asm__ volatile(
        "lr.w t1, 0(%1)\n\t"
        "sc.w %0, %2, 0(%1)\n\t"
        : "=&r" (rd)
        : "r" (addr), "r" (val)
        : "t1", "memory");
    return rd;
}

int main(void) {
    unsigned int before_a, before_b, after_a, after_b;
    unsigned long rd_a, rd_b, rd_c;

    uart_init();
    uart_puts("sc-fail: store-conditional failure-path experiment\n");
    uart_puts("hart mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    // Install the trap vector (direct mode) and arm mscratch. No trap
    // is expected; the handler halts the hart, so a trap would end
    // the run without printing RESULT: PASS.
    __asm__ volatile("csrw mtvec, %0" :: "r"(scf_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(scf_save));
    uart_puts("trap vector installed (halts the hart on any trap)\n");

    cell_a = 0xAAAAAAAAU;
    cell_b = 0xBBBBBBBBU;

    // Experiment (a): sc.w with no preceding lr.w on cell_a.
    before_a = cell_a;
    rd_a = sc_no_lr(&cell_a, 0x12345678U);
    after_a = cell_a;
    uart_puts("test(a) sc.w with no lr.w:\n");
    uart_puts("  before=");
    uart_put_hex(before_a);
    uart_puts(" rd=");
    uart_put_dec(rd_a);
    uart_puts(" after=");
    uart_put_hex(after_a);
    uart_puts("\n");
    check(rd_a != 0, "test(a): sc without lr reported success (rd == 0)");
    check(after_a == before_a, "test(a): memory word changed");

    // Experiment (b): lr.w on cell_a, then sc.w on cell_b.
    before_a = cell_a;
    before_b = cell_b;
    rd_b = lr_then_sc_other(&cell_a, &cell_b, 0x9ABCDEF0U);
    after_a = cell_a;
    after_b = cell_b;
    uart_puts("test(b) lr.w on A, sc.w on B (different address):\n");
    uart_puts("  before_a=");
    uart_put_hex(before_a);
    uart_puts(" before_b=");
    uart_put_hex(before_b);
    uart_puts("\n  rd=");
    uart_put_dec(rd_b);
    uart_puts(" after_a=");
    uart_put_hex(after_a);
    uart_puts(" after_b=");
    uart_put_hex(after_b);
    uart_puts("\n");
    check(rd_b != 0, "test(b): sc on wrong address reported success");
    check(after_a == before_a, "test(b): cell_a changed");
    check(after_b == before_b, "test(b): cell_b changed");

    // Experiment (c): control, lr.w then sc.w on the same cell.
    rd_c = lr_then_sc_same(&cell_a, 0x12345678U);
    after_a = cell_a;
    uart_puts("test(c) control: lr.w then sc.w on same cell:\n");
    uart_puts("  rd=");
    uart_put_dec(rd_c);
    uart_puts(" after_a=");
    uart_put_hex(after_a);
    uart_puts("\n");
    check(rd_c == 0, "test(c): control pair did not report success");
    check(after_a == 0x12345678U, "test(c): control store not visible");

    // Trap accounting: zero traps must have fired across all tests.
    uart_puts("trap seen-flag=");
    uart_put_dec(scf_save[3]);
    uart_puts("\n");
    check(scf_save[3] == 0, "unexpected trap fired during the run");
    if (scf_save[3] != 0) {
        uart_puts("  trap mcause=");
        uart_put_hex(scf_save[0]);
        uart_puts(" mepc=");
        uart_put_hex(scf_save[1]);
        uart_puts(" mtval=");
        uart_put_hex(scf_save[2]);
        uart_puts("\n");
    }

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
