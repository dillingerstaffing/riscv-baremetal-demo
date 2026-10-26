// sltiu_main.c: firsthand check that sltiu against 1 is a branchless zero test.
//
// sltiu compares unsigned: rd = 1 when rs < imm, else 0. With imm = 1,
// the only unsigned value below 1 is 0, so the result is 1 exactly when
// the input is zero. The assembler names this idiom seqz.
#include "uart.h"

static unsigned long zero_test(unsigned long v) {
    unsigned long r;
    __asm__ volatile("sltiu %0, %1, 1" : "=r" (r) : "r" (v));
    return r;
}

int main(void) {
    static const unsigned long vals[5] = {0, 1, 2, 5, 0xffffffffffffffffUL};
    unsigned long i;
    uart_init();
    for (i = 0; i < 5; i++) {
        uart_puts("sltiu(v, 1) v=");
        uart_put_hex(vals[i]);
        uart_puts(" -> ");
        uart_put_dec(zero_test(vals[i]));
        uart_puts("\n");
    }
    uart_puts("done\n");
    for (;;) { __asm__ volatile("wfi"); }
    return 0;
}
