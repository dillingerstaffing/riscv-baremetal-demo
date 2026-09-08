// pmain.c: bring-up for the preemptive scheduler demo. Shares boot.S and
// the UART driver with the cooperative demo; everything timer/trap related
// lives in this module.

#include "../uart.h"
#include "clint.h"
#include "psched.h"
#include "ptasks.h"

int main(void) {
    uart_init();

    uart_puts("\n========================================\n");
    uart_puts("riscv-baremetal-demo: preemptive scheduler\n");
    uart_puts("CLINT timer interrupt + full-context switch\n");
    uart_puts("========================================\n\n");

    uart_puts("clint: mtime@");
    uart_put_hex(0x0200bff8UL);
    uart_puts(" mtimecmp@");
    uart_put_hex(0x02004000UL);
    uart_puts(" timebase ");
    uart_put_dec(CLINT_TICKS_PER_SEC);
    uart_puts(" Hz (100 ns/tick)\n");
    uart_puts("quantum 10000 ticks (1 ms), 200 ticks total\n\n");

    ptasks_register();
    preempt_init();

    uart_puts("timer armed, entering wfi wait...\n");
    preempt_wait();
    preempt_stop();

    preempt_report();

    uart_puts("\npreempt: halting.\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
    return 0;
}
