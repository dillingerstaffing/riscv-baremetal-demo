// sh_main.c: entry point for the UART shell module. Installs the
// trap handler (mscratch/trap_bank, mtvec/trap_entry), then runs the
// command interpreter forever.

#include "../uart.h"
#include "cmds.h"

extern void trap_entry(void);
extern unsigned long trap_bank[34];

static void install_trap_handler(void) {
    __asm__ volatile("csrw mscratch, %0" ::"r"(trap_bank));
    __asm__ volatile("csrw mtvec, %0" ::"r"(trap_entry));
}

int main(void) {
    uart_init();
    install_trap_handler();
    shell_run();
    return 0;  // unreachable; keeps the signature honest
}
