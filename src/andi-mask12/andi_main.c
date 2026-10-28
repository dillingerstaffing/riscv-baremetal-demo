#include "uart.h"
static unsigned long andi2047(unsigned long v){ unsigned long r; __asm__ volatile("andi %0, %1, 2047" : "=r"(r) : "r"(v)); return r; }
static unsigned long maskfff(unsigned long v){ unsigned long r; __asm__ volatile("slli %0, %1, 52\n\tsrli %0, %0, 52" : "=r"(r) : "r"(v)); return r; }
static unsigned long andfff(unsigned long v){ unsigned long r; __asm__ volatile("li t0, 4095\n\tand %0, %1, t0" : "=r"(r) : "r"(v) : "t0"); return r; }
int main(void){
    static const unsigned long vals[4] = {0x12345abcUL, 0xfffUL, 0x1000UL, 0xffffUL};
    unsigned long i; uart_init();
    for (i = 0; i < 4; i++){
        uart_puts("v="); uart_put_hex(vals[i]);
        uart_puts(" andi2047="); uart_put_hex(andi2047(vals[i]));
        uart_puts(" shiftfff="); uart_put_hex(maskfff(vals[i]));
        uart_puts(" andfff="); uart_put_hex(andfff(vals[i])); uart_puts("\n");
    }
    uart_puts("done\n"); for (;;) { __asm__ volatile("wfi"); } return 0;
}
