// stimer.h: timer rearm for the smode module.
//
// The virt board's CLINT sits at 0x02000000: mtime at offset 0xbff8,
// mtimecmp for hart 0 at offset 0x4000, timebase 10 MHz (100 ns per tick,
// from QEMU's own device tree).

#ifndef STIMER_H
#define STIMER_H

#define STIMER_TICKS_PER_SEC 10000000UL  // 10 MHz timebase -> 100 ns per tick
#define STIMER_NS_PER_TICK 100UL

// 1 if the hart implements Sstc (the stimecmp CSR), probed by m_boot in
// M-mode before the drop to S-mode.
extern int g_use_sstc;

// Arm the next tick one quantum in the future and remember its deadline.
// S-mode binary: writes stimecmp when Sstc is present, otherwise issues
// an ecall asking M-mode to reprogram mtimecmp (which S-mode cannot
// touch). M-mode baseline: writes mtimecmp directly.
void stimer_arm(void);

// Disarm the timer: move the comparator far into the future so no
// interrupt is pending. Used on the final tick and in the defensive
// guard, so that returning to the boot context cannot immediately
// re-trap on a stale pending bit.
void stimer_disarm(void);

// The comparator deadline the most recent arm programmed; the handler
// subtracts this from the entry mtime stamp to get interrupt latency.
unsigned long stimer_deadline(void);

#endif  // STIMER_H
