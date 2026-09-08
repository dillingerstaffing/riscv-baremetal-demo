// ptasks.c: three tasks that never yield. Each one spins on a linear
// congruential step plus a progress counter, all through volatile globals
// so the compiler cannot delete or hoist the work. If the scheduler were
// cooperative, the first task would spin forever and the other two
// counters would stay at zero.

#include "ptasks.h"
#include "psched.h"

static volatile unsigned long state_a = 1, state_b = 2, state_c = 3;
static volatile unsigned long iters_a, iters_b, iters_c;

void ptask_a(void) {
    for (;;) {
        state_a = state_a * 6364136223846793005UL + 1442695040888963407UL;
        iters_a++;
    }
}

void ptask_b(void) {
    for (;;) {
        state_b = state_b * 2862933555777941757UL + 3037000493UL;
        iters_b++;
    }
}

void ptask_c(void) {
    for (;;) {
        state_c = state_c * 1597334677UL + 1013904223UL;
        iters_c++;
    }
}

void ptasks_register(void) {
    ptask_create(ptask_a, "spin-a");
    ptask_create(ptask_b, "spin-b");
    ptask_create(ptask_c, "spin-c");
}

int ptask_count(void) {
    return 3;
}

unsigned long ptask_iters(int i) {
    if (i == 0)
        return iters_a;
    if (i == 1)
        return iters_b;
    return iters_c;
}

const char *ptask_name(int i) {
    if (i == 0)
        return "spin-a";
    if (i == 1)
        return "spin-b";
    return "spin-c";
}
