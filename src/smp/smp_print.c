// smp_print.c: UART output helpers that hold the spinlock for the whole
// call, so a line printed by one hart is never interleaved with output
// from the other hart.

#include "../uart.h"
#include "smp.h"

spinlock_t uart_lock = {0};

void locked_puts(const char *s) {
    spin_lock(&uart_lock);
    uart_puts(s);
    spin_unlock(&uart_lock);
}

// Prints "label=value\n", decimal or hex.
void locked_print_kv(const char *label, unsigned long v, int hex) {
    spin_lock(&uart_lock);
    uart_puts(label);
    uart_putc('=');
    if (hex)
        uart_put_hex(v);
    else
        uart_put_dec(v);
    uart_putc('\n');
    spin_unlock(&uart_lock);
}

// Prints "a=va b=vb\n", decimal or hex.
void locked_print_line2(const char *a, unsigned long va, const char *b,
                        unsigned long vb, int hex) {
    spin_lock(&uart_lock);
    uart_puts(a);
    uart_putc('=');
    if (hex)
        uart_put_hex(va);
    else
        uart_put_dec(va);
    uart_putc(' ');
    uart_puts(b);
    uart_putc('=');
    if (hex)
        uart_put_hex(vb);
    else
        uart_put_dec(vb);
    uart_putc('\n');
    spin_unlock(&uart_lock);
}
