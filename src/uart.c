// uart.c: ns16550a UART driver. The virt board maps UART0 at 0x10000000.
// QEMU already configures the baud rate, so init only selects 8N1 framing.
// All output is polled (no interrupts), which keeps the driver dependency
// free and easy to reuse in other bare-metal projects.

#include "uart.h"

#define UART0_BASE 0x10000000UL

#define UART_THR 0x00  // transmitter holding register (write)
#define UART_LSR 0x05  // line status register
#define UART_LCR 0x03  // line control register

#define LSR_THRE (1 << 5)  // transmitter holding register empty
#define LSR_DR (1 << 0)    // receiver data ready

static volatile unsigned char *uart_reg(unsigned long off) {
    return (volatile unsigned char *)(UART0_BASE + off);
}

void uart_init(void) {
    // 8 data bits, no parity, one stop bit (8N1). DLAB stays 0.
    *uart_reg(UART_LCR) = 0x03;
}

void uart_putc(char c) {
    if (c == '\n')
        uart_putc('\r');  // CRLF for clean terminal output
    while ((*uart_reg(UART_LSR) & LSR_THRE) == 0)
        ;  // wait until the transmitter is ready
    *uart_reg(UART_THR) = (unsigned char)c;
}

void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

static void uart_put_uint(unsigned long v, unsigned base, const char *digits) {
    char buf[24];
    int i = 0;

    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    while (i > 0)
        uart_putc(buf[--i]);
}

void uart_put_hex(unsigned long v) {
    uart_puts("0x");
    uart_put_uint(v, 16, "0123456789abcdef");
}

void uart_put_dec(unsigned long v) {
    uart_put_uint(v, 10, "0123456789");
}

char uart_getc(void) {
    // Block until the receiver holds a byte, then read it. Same
    // polling discipline as the transmit path above.
    while ((*uart_reg(UART_LSR) & LSR_DR) == 0)
        ;
    return (char)*uart_reg(UART_THR);  // receive buffer lives at offset 0
}
