// jalr_main.c: jalr clears bit 0 of its jump target on RV64.
//
// A jump target formed by adding 1 to a label address is odd. The test
// jumps to that odd address with jalr and checks that the code at the
// even label still runs and returns. If bit 0 survived, the hart would
// fetch from an odd address and never reach the label.
//
// t2 is the link register, so ra stays untouched. The target stores 1
// into landed, then returns through t2. The instruction after the jalr
// is a jump over the target block, so the fall-through path cannot reach
// the label by accident. Both printed addresses come out of the same
// asm block that performs the jump.

#include "uart.h"

static volatile int landed = 0;

int main(void) {
    unsigned long target = 0, odd = 0;
    int mismatches = 0;

    uart_init();

    __asm__ volatile(
        "la %0, 1f\n\t"
        "addi %1, %0, 1\n\t"
        "jalr t2, 0(%1)\n\t"
        "j 2f\n"
        "1:\n\t"
        "li t1, 1\n\t"
        "sw t1, 0(%2)\n\t"
        "jalr x0, 0(t2)\n"
        "2:\n\t"
        : "=r"(target), "=r"(odd)
        : "r"(&landed)
        : "t1", "t2", "memory");

    uart_puts("target: ");
    uart_put_hex(target);
    uart_puts("\n");
    uart_puts("odd:    ");
    uart_put_hex(odd);
    uart_puts("\n");
    uart_puts("landed: ");
    uart_put_dec((unsigned long)landed);
    uart_puts("\n");

    // The ISA contract: jalr sets bit 0 of (rs1 + imm) to zero.
    if ((odd & 1UL) != 1UL) {
        uart_puts("FAIL: odd address is even\n");
        mismatches++;
    }
    if ((odd & ~1UL) != target) {
        uart_puts("FAIL: cleared odd != target\n");
        mismatches++;
    }
    if ((target & 1UL) != 0UL) {
        uart_puts("FAIL: target is odd\n");
        mismatches++;
    }
    if (landed != 1) {
        uart_puts("FAIL: target block did not run\n");
        mismatches++;
    }

    uart_puts("mismatches: ");
    uart_put_dec((unsigned long)mismatches);
    uart_puts("\n");
    uart_puts(mismatches == 0 ? "PASS\n" : "FAIL\n");
    return 0;
}
