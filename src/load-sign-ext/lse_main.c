// lse_main.c: lw sign-extends, lwu zero-extends a 32-bit word load on RV64.
//
// A 32-bit pattern with the top bit set (0x80000001) is stored to a scratch
// word, then read back once with lw and once with lwu. The run checks the
// exact register values against the ISA contract and demonstrates the bench
// consequence: the lw result fails an == comparison against the original
// 32-bit pattern, the lwu result passes it.

#include "uart.h"

static volatile unsigned int scratch __attribute__((aligned(4)));

int main(void) {
    unsigned long r_lw = 0, r_lwu = 0;
    unsigned int pat = 0x80000001u;
    int mismatches = 0;

    uart_init();

    uart_puts("pattern: ");
    uart_put_hex((unsigned long)pat);
    uart_puts("\n");

    __asm__ volatile("sw %0, 0(%1)" :: "r"(pat), "r"(&scratch) : "memory");
    __asm__ volatile("lw %0, 0(%1)" : "=r"(r_lw) : "r"(&scratch) : "memory");
    __asm__ volatile("lwu %0, 0(%1)" : "=r"(r_lwu) : "r"(&scratch) : "memory");

    uart_puts("lw  -> ");
    uart_put_hex(r_lw);
    uart_puts("\n");
    uart_puts("lwu -> ");
    uart_put_hex(r_lwu);
    uart_puts("\n");

    // The ISA contract: lw sign-extends bit 31; lwu zero-extends.
    if (r_lw != 0xffffffff80000001UL) {
        uart_puts("FAIL: lw result\n");
        mismatches++;
    }
    if (r_lwu != 0x0000000080000001UL) {
        uart_puts("FAIL: lwu result\n");
        mismatches++;
    }

    // The bench consequence: comparing the lw result against the 32-bit
    // pattern silently fails; the lwu result matches.
    if ((r_lw == (unsigned long)pat)) {
        uart_puts("FAIL: lw == pattern unexpectedly true\n");
        mismatches++;
    }
    if (!(r_lwu == (unsigned long)pat)) {
        uart_puts("FAIL: lwu == pattern unexpectedly false\n");
        mismatches++;
    }

    uart_puts("mismatches: ");
    uart_put_dec((unsigned long)mismatches);
    uart_puts("\n");
    uart_puts(mismatches == 0 ? "PASS\n" : "FAIL\n");
    return 0;
}
