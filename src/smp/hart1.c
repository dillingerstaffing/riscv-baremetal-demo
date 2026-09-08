// hart1.c: the secondary hart's entry point.
//
// hart1_main runs on hart 1 after hart 0 raises smp_release. Its first
// action is a cycle-counter stamp, so the delta between hart 0's
// release stamp and this stamp is the bring-up latency. It prints its
// hart ID (read from the mhartid CSR) and its stack pointer under the
// UART spinlock, records its sp for hart 0's overlap check, then
// signals completion.

#include "../uart.h"
#include "smp.h"

volatile unsigned long t1_entry_cycles;
volatile unsigned long t1_printed_cycles;
volatile unsigned long t1_sp;
volatile int hart1_done;

void hart1_main(void) {
    // First action after release: stamp the cycle counter.
    t1_entry_cycles = rdcycle();
    mem_fence();

    unsigned long hartid = read_mhartid();
    unsigned long sp = read_sp();
    t1_sp = sp;

    unsigned long base = (unsigned long)&hart_stacks[1][0];
    unsigned long top = base + HART_STACK_SIZE;

    locked_puts("hart1: secondary hart alive\n");
    locked_print_kv("hart1: mhartid", hartid, 0);
    locked_print_line2("hart1: stack_base", base, "stack_top", top, 1);
    locked_print_kv("hart1: sp", sp, 1);

    t1_printed_cycles = rdcycle();

    // Publish completion after all writes above are visible.
    mem_fence();
    hart1_done = 1;

    for (;;) {
        __asm__ volatile("wfi");
    }
}
