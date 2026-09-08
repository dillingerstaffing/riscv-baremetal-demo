// spinlock.c: amoswap-based spinlock for serializing UART output.

#include "spinlock.h"

void spin_lock(spinlock_t *l) {
    int old;
    __asm__ volatile(
        "li %0, 1\n"
        "1: amoswap.w.aq %0, %0, (%1)\n"
        "   bnez %0, 1b"
        : "=&r"(old)
        : "r"(&l->locked)
        : "memory");
}

void spin_unlock(spinlock_t *l) {
    __asm__ volatile(
        "fence rw, w\n"
        "sw zero, 0(%0)"
        :
        : "r"(&l->locked)
        : "memory");
}
