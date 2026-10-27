// addiw_main.c: firsthand check that addiw with immediate 0 sign-extends
// the low 32 bits, while addi with immediate 0 leaves all 64 bits alone.
//
// addiw takes bits 31:0 of rs1, adds the sign-extended 12-bit immediate,
// then sign-extends the 32-bit result to 64 bits. With immediate 0 the
// addition is a no-op, so the whole instruction is just: take the low
// word, sign-extend it. The assembler names this idiom sext.w. addi has
// no such truncation: it adds to the full 64-bit value.
#include "uart.h"

static unsigned long addiw_zero(unsigned long v) {
    unsigned long r;
    __asm__ volatile("addiw %0, %1, 0" : "=r" (r) : "r" (v));
    return r;
}

static unsigned long addi_zero(unsigned long v) {
    unsigned long r;
    __asm__ volatile("addi %0, %1, 0" : "=r" (r) : "r" (v));
    return r;
}

int main(void) {
    static const unsigned long vals[5] = {
        0x0000000080000001UL,
        0x000000007fffffffUL,
        0x0000000180000001UL,
        0xffffffff80000001UL,
        0x00000000ffffffffUL,
    };
    unsigned long i;
    uart_init();
    for (i = 0; i < 5; i++) {
        uart_puts("sextw in=");
        uart_put_hex(vals[i]);
        uart_puts(" addiw=");
        uart_put_hex(addiw_zero(vals[i]));
        uart_puts(" addi=");
        uart_put_hex(addi_zero(vals[i]));
        uart_puts("\n");
    }
    uart_puts("done\n");
    for (;;) { __asm__ volatile("wfi"); }
    return 0;
}
