// clint.h: SiFive CLINT (core-local interruptor) driver for QEMU's virt board.
//
// The virt board maps the CLINT at 0x02000000: mtimecmp for hart 0 lives at
// offset 0x4000, mtime at offset 0xbff8. The board's device tree reports
// timebase-frequency = 10000000, so one mtime tick is 100 ns. A machine
// timer interrupt becomes pending when mtime >= mtimecmp.

#ifndef CLINT_H
#define CLINT_H

#define CLINT_TICKS_PER_SEC 10000000UL  // 10 MHz timebase -> 100 ns per tick
#define CLINT_NS_PER_TICK 100UL

unsigned long clint_get_mtime(void);
void clint_set_mtimecmp(unsigned long v);
unsigned long clint_get_mtimecmp(void);

#endif  // CLINT_H
