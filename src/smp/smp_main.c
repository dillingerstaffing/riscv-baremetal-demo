// smp_main.c: the primary hart's bring-up.
//
// Hart 0 initializes the UART, prints its own hart ID and stack range,
// stamps the cycle counter, releases hart 1 via the smp_release flag,
// waits for hart 1's completion signal (with a cycle-count timeout so a
// stuck secondary is reported instead of hanging), then prints the
// verification report: both hart IDs, bring-up latency in cycles, and
// the stack-overlap check.

#include "../uart.h"
#include "smp.h"

char hart_stacks[NHART][HART_STACK_SIZE] __attribute__((aligned(16)));
volatile int smp_release = 0;
volatile unsigned long t_release_cycles;

// Wait bound: 15e9 cycle-counter units. On this QEMU the counter runs
// at ~1.5e9 units per second of virtual time, so this is ~10 s of
// virtual time, far beyond any plausible bring-up.
#define HART1_TIMEOUT_CYCLES 15000000000UL

void smp_main(void) {
    uart_init();

    locked_puts("smp: 2-hart bring-up on QEMU virt\n");

    unsigned long hartid = read_mhartid();
    locked_print_kv("hart0: mhartid", hartid, 0);

    unsigned long base0 = (unsigned long)&hart_stacks[0][0];
    unsigned long top0 = base0 + HART_STACK_SIZE;
    unsigned long sp0 = read_sp();
    locked_print_line2("hart0: stack_base", base0, "stack_top", top0, 1);
    locked_print_kv("hart0: sp", sp0, 1);

    // Stamp, then release hart 1. The fence orders the stamp write
    // before the flag write.
    t_release_cycles = rdcycle();
    mem_fence();
    smp_release = 1;
    locked_puts("hart0: released hart 1\n");

    // Wait for hart 1 to finish, with a timeout.
    unsigned long deadline = t_release_cycles + HART1_TIMEOUT_CYCLES;
    while (!hart1_done && rdcycle() < deadline)
        mem_fence();

    if (!hart1_done) {
        locked_puts("hart0: TIMEOUT waiting for hart 1\n");
        for (;;) {
            __asm__ volatile("wfi");
        }
    }

    unsigned long t_done = rdcycle();

    // Verification report. All numbers are measurements or address
    // arithmetic on the static stack array.
    locked_puts("smp: verification report\n");
    locked_print_kv("hart0: release_cycles", t_release_cycles, 0);
    locked_print_kv("hart1: entry_cycles", t1_entry_cycles, 0);
    locked_print_kv("hart1: printed_cycles", t1_printed_cycles, 0);
    locked_print_kv("hart0: done_cycles", t_done, 0);

    unsigned long bringup_lat =
        t1_entry_cycles - t_release_cycles;  // release -> hart1 first stamp
    unsigned long entry_to_print = t1_printed_cycles - t1_entry_cycles;
    locked_print_kv("bring-up latency (release->hart1 entry), cycles",
                    bringup_lat, 0);
    locked_print_kv("hart1 entry->print done, cycles", entry_to_print, 0);

    // Stack overlap check: the two slices are adjacent 4 KiB regions,
    // so disjointness is address arithmetic; sp-in-range is a
    // measurement per hart.
    unsigned long base1 = (unsigned long)&hart_stacks[1][0];
    unsigned long top1 = base1 + HART_STACK_SIZE;
    int disjoint = (top0 <= base1) || (top1 <= base0);
    int sp0_ok = (sp0 >= base0) && (sp0 <= top0);
    int sp1_ok = (t1_sp >= base1) && (t1_sp <= top1);
    locked_print_kv("stack slices disjoint (1=yes)", (unsigned long)disjoint,
                    0);
    locked_print_kv("hart0 sp in own slice (1=yes)", (unsigned long)sp0_ok,
                    0);
    locked_print_kv("hart1 sp in own slice (1=yes)", (unsigned long)sp1_ok,
                    0);

    locked_puts("smp: both harts ran, done\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}
