// smp.h: shared definitions for the two-hart SMP bring-up module.

#ifndef SMP_H
#define SMP_H

#include "spinlock.h"

#define NHART 2
#define HART_STACK_SIZE 4096

// Per-hart stacks: hart N owns hart_stacks[N] (4 KiB), growing down from
// hart_stacks[N] + HART_STACK_SIZE. Lives in .bss; both harts set sp
// from it in smp_boot.S before hart 0 clears BSS.
extern char hart_stacks[NHART][HART_STACK_SIZE];

// Release flag for the secondary hart. In .data (explicit initializer)
// so it reads 0 from the loaded image even before hart 0 clears BSS.
extern volatile int smp_release;

// Set by hart 1 when it has finished its work.
extern volatile int hart1_done;

// cycle-counter stamps. t_release_cycles is written by hart 0 just
// before it raises smp_release; t1_entry_cycles is hart 1's first
// action after observing the flag; t1_printed_cycles is stamped after
// hart 1 finishes its locked UART output; t1_sp records hart 1's stack
// pointer so hart 0 can check it against hart 1's stack slice.
extern volatile unsigned long t_release_cycles;
extern volatile unsigned long t1_entry_cycles;
extern volatile unsigned long t1_printed_cycles;
extern volatile unsigned long t1_sp;

// UART output lock: both harts hold it for a whole line so lines from
// the two harts never interleave mid-line.
extern spinlock_t uart_lock;

static inline unsigned long rdcycle(void) {
    unsigned long v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

static inline unsigned long rdtime(void) {
    unsigned long v;
    __asm__ volatile("rdtime %0" : "=r"(v));
    return v;
}

static inline unsigned long read_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

static inline unsigned long read_sp(void) {
    unsigned long v;
    __asm__ volatile("mv %0, sp" : "=r"(v));
    return v;
}

static inline void mem_fence(void) {
    __asm__ volatile("fence" ::: "memory");
}

// UART helpers that hold the lock for the whole call. Implemented in
// smp_print.c; usable from both harts.
void locked_puts(const char *s);
void locked_print_kv(const char *label, unsigned long v, int hex);
void locked_print_line2(const char *a, unsigned long va, const char *b,
                        unsigned long vb, int hex);

#endif  // SMP_H
