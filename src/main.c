// main.c: board bring-up, task registration, scheduler start.

#include "uart.h"
#include "sched.h"
#include "tasks.h"

int main(void) {
    uart_init();

    uart_puts("\n========================================\n");
    uart_puts("riscv-baremetal-demo: QEMU virt (rv64imac)\n");
    uart_puts("UART driver + cooperative round-robin scheduler\n");
    uart_puts("========================================\n\n");

    tasks_register();
    uart_puts("scheduler: starting 3 tasks...\n\n");

    sched_run();

    uart_puts("\nscheduler: all tasks finished. halting.\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
    return 0;
}
