// smain.c: bring-up for the smode module. Shared by both binaries; the
// S-mode binary reaches main via s_entry after m_boot's sret, the M-mode
// baseline falls through to main from _start still in M-mode.

#include "../uart.h"
#include "stimer.h"
#include "ssched.h"
#include "stasks.h"

int main(void) {
    uart_init();

    uart_puts("\n========================================\n");
#if TRAP_SMODE
    uart_puts("riscv-baremetal-demo: S-mode trap delegation\n");
    uart_puts("scheduler on the delegated supervisor timer interrupt\n");
#else
    uart_puts("riscv-baremetal-demo: M-mode baseline\n");
    uart_puts("same scheduler on the machine timer interrupt\n");
#endif
    uart_puts("========================================\n\n");

#if TRAP_SMODE
    uart_puts("Sstc (stimecmp): ");
    uart_puts(g_use_sstc ? "present, pure S-mode timer\n"
                         : "absent, M-mode ecall rearm\n");
#endif
    uart_puts("quantum 10000 ticks (1 ms), 200 ticks total\n\n");

    stasks_register();
    sched_init();

    uart_puts("timer armed, waiting for ticks...\n");
    sched_wait();
    sched_stop();

    sched_report();

    uart_puts("\nhalting.\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
    return 0;
}
