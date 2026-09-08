// clint.c: direct MMIO access to the CLINT timer registers. A single 64-bit
// store programs the whole compare register; QEMU's virt CLINT accepts it.

#include "clint.h"

#define CLINT_BASE 0x02000000UL
#define CLINT_MTIMECMP_HART0 (CLINT_BASE + 0x4000UL)
#define CLINT_MTIME (CLINT_BASE + 0xbff8UL)

unsigned long clint_get_mtime(void) {
    return *(volatile unsigned long *)CLINT_MTIME;
}

void clint_set_mtimecmp(unsigned long v) {
    *(volatile unsigned long *)CLINT_MTIMECMP_HART0 = v;
}

unsigned long clint_get_mtimecmp(void) {
    return *(volatile unsigned long *)CLINT_MTIMECMP_HART0;
}
