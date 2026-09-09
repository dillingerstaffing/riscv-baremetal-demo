// stimer.c: timer rearm for the smode module.

#include "stimer.h"

#define QUANTUM_TICKS 10000UL  // 1 ms per slice at the 10 MHz timebase
#define CLINT_MTIME 0x0200bff8UL
#define CLINT_MTIMECMP 0x02004000UL

// M-mode ecall service numbers (handled by m_trap_entry in sboot.S).
#define ECALL_REARM 1  // reprogram mtimecmp; deadline in a1

int g_use_sstc = 0;
static unsigned long deadline = 0;

static unsigned long read_mtime(void) {
    unsigned long v;
    // rdtime is readable in S-mode; it tracks the mtime MMIO register
    // (verified in the preempt module's PROOF: back-to-back reads land
    // within 1-2 ticks of the MMIO reads around them).
    __asm__ volatile("rdtime %0" : "=r"(v));
    return v;
}

void stimer_arm(void) {
    deadline = read_mtime() + QUANTUM_TICKS;
    // If a handler ever ran longer than a whole quantum, skip ahead so
    // the still-pending interrupt does not re-fire immediately in a
    // tight loop.
    while (deadline <= read_mtime())
        deadline += QUANTUM_TICKS;
#if TRAP_SMODE
    if (g_use_sstc) {
        // Sstc: S-mode programs stimecmp directly; the supervisor timer
        // interrupt fires when time >= stimecmp.
        __asm__ volatile("csrw 0x14d, %0" :: "r"(deadline));
    } else {
        // No Sstc: mtimecmp is M-mode-only, so ask M-mode via ecall.
        // Supervisor ecalls are not delegated (see msetup.c), so this
        // traps to the M-mode vector, which reprograms mtimecmp,
        // advances mepc past the ecall, and returns with mret.
        register unsigned long a0 __asm__("a0") = ECALL_REARM;
        register unsigned long a1 __asm__("a1") = deadline;
        __asm__ volatile("ecall" : "+r"(a0) : "r"(a1) : "memory");
    }
#else
    *(volatile unsigned long *)CLINT_MTIMECMP = deadline;
#endif
}

unsigned long stimer_deadline(void) {
    return deadline;
}

void stimer_disarm(void) {
    // Far future: clears the pending bit (interrupt fires when
    // mtime >= comparator, so ~0 never fires within the run).
    deadline = ~0UL;
#if TRAP_SMODE
    if (g_use_sstc) {
        __asm__ volatile("csrw 0x14d, %0" :: "r"(~0UL));
    } else {
        // Same ecall service as the rearm; the deadline just happens
        // to be the far future.
        register unsigned long a0 __asm__("a0") = ECALL_REARM;
        register unsigned long a1 __asm__("a1") = ~0UL;
        __asm__ volatile("ecall" : "+r"(a0) : "r"(a1) : "memory");
    }
#else
    *(volatile unsigned long *)CLINT_MTIMECMP = ~0UL;
#endif
}
