// uart.h: minimal driver for the ns16550a UART on the QEMU virt board.
#ifndef UART_H
#define UART_H

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_put_hex(unsigned long v);
void uart_put_dec(unsigned long v);
char uart_getc(void);

#endif  // UART_H
