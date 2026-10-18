// bba_main.c: blt compares signed, bltu compares unsigned, on RV64.
//
// Two addresses, one with bit 63 set and one without, are compared with
// the signed branch (blt) and the unsigned branch (bltu). The run records
// which branch is taken for each pair and checks the ISA contract:
//   blt  rs1, rs2 -> branch if (int64_t)rs1 < (int64_t)rs2
//   bltu rs1, rs2 -> branch if (uint64_t)rs1 < (uint64_t)rs2
// The bench consequence: a signed range check on an address inverts the
// address map above 0x7fffffffffffffff.

#include "uart.h"

// Return 1 if the blt branch is taken for (a, b), 0 otherwise.
static int blt_taken(unsigned long a, unsigned long b) {
    int r;
    __asm__ volatile(
        "blt %1, %2, 1f\n\t"
        "li %0, 0\n\t"
        "j 2f\n"
        "1:\n\t"
        "li %0, 1\n"
        "2:"
        : "=r"(r)
        : "r"(a), "r"(b));
    return r;
}

// Return 1 if the bltu branch is taken for (a, b), 0 otherwise.
static int bltu_taken(unsigned long a, unsigned long b) {
    int r;
    __asm__ volatile(
        "bltu %1, %2, 1f\n\t"
        "li %0, 0\n\t"
        "j 2f\n"
        "1:\n\t"
        "li %0, 1\n"
        "2:"
        : "=r"(r)
        : "r"(a), "r"(b));
    return r;
}

static void check(const char *name, int got, int want, int *mismatches) {
    uart_puts(name);
    uart_puts(": got ");
    uart_put_dec((unsigned long)got);
    uart_puts(", want ");
    uart_put_dec((unsigned long)want);
    if (got != want) {
        uart_puts("  FAIL\n");
        (*mismatches)++;
    } else {
        uart_puts("  ok\n");
    }
}

int main(void) {
    int mismatches = 0;
    unsigned long hi = 0x8000000000000000UL;  // bit 63 set: negative as int64_t
    unsigned long lo = 0x0000000000001000UL;  // small positive either way
    unsigned long kern = 0xFFFFFFFFFFFFF000UL; // high-half address: -4096 as int64_t

    uart_init();

    uart_puts("hi = ");
    uart_put_hex(hi);
    uart_puts("\nlo = ");
    uart_put_hex(lo);
    uart_puts("\nkern = ");
    uart_put_hex(kern);
    uart_puts("\n");

    // Signed compare: hi is negative, lo is positive.
    check("blt (hi, lo) ", blt_taken(hi, lo), 1, &mismatches);
    check("blt (lo, hi) ", blt_taken(lo, hi), 0, &mismatches);
    // Unsigned compare: hi is the larger address.
    check("bltu(hi, lo) ", bltu_taken(hi, lo), 0, &mismatches);
    check("bltu(lo, hi) ", bltu_taken(lo, hi), 1, &mismatches);

    // The applied case: a high-half address against a low limit.
    // Signed, kern reads as -4096, which is below lo: the map inverts.
    check("blt (kern, lo)", blt_taken(kern, lo), 1, &mismatches);
    // Unsigned, kern is the top of the map: above lo.
    check("bltu(kern, lo)", bltu_taken(kern, lo), 0, &mismatches);

    uart_puts("mismatches: ");
    uart_put_dec((unsigned long)mismatches);
    uart_puts("\n");
    uart_puts(mismatches == 0 ? "PASS\n" : "FAIL\n");
    return 0;
}
