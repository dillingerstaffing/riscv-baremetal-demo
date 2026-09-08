// spinlock.h: a mutual-exclusion lock built on one atomic instruction.
//
// The lock word is a single 32-bit int. Acquire swaps in a 1 with
// amoswap.w.aq; the swap is atomic across harts, so exactly one hart
// observes the old value 0 and wins. Release stores 0 after a fence.

#ifndef SPINLOCK_H
#define SPINLOCK_H

typedef struct {
    volatile int locked;
} spinlock_t;

void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);

#endif  // SPINLOCK_H
