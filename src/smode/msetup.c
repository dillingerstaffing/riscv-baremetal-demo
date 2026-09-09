// msetup.c: M-mode one-time setup for the smode module.
//
// TRAP_SMODE=1 (smode.elf): probe for the Sstc extension, write medeleg /
// mideleg so supervisor traps and interrupts route to S-mode, install stvec
// (the S-mode scheduler vector) plus a minimal M-mode vector that serves
// only the timer-rearm ecall, set sstatus.SPP and mstatus.MPP for the drop
// to S-mode, then sret into s_entry. Never returns.
//
// TRAP_SMODE=0 (smode-mbase.elf): stub. The baseline runs entirely in
// M-mode, so no delegation is programmed.

#include "ssched.h"

#if TRAP_SMODE

// stimecmp (Sstc) CSR number; menvcfg bit 63 (STCE) enables it.
#define CSR_STIMECMP 0x14d
#define MENVCFG_STCE (1UL << 63)

// Exceptions delegated to S-mode via medeleg: illegal instruction (2),
// breakpoint (3), user ecall (8), instruction/load/store page faults
// (12,13,15). Supervisor ecall (9) is deliberately NOT delegated: the
// S-mode scheduler uses it to ask M-mode to reprogram mtimecmp on harts
// without the Sstc extension.
#define MEDELEG_MASK ((1UL<<2)|(1UL<<3)|(1UL<<8)|(1UL<<12)|(1UL<<13)|(1UL<<15))
// Interrupts delegated to S-mode via mideleg: SSI (1), STI (5), SEI (9).
#define MIDELEG_MASK ((1UL<<1)|(1UL<<5)|(1UL<<9))

#define CLINT_MTIMECMP 0x02004000UL

extern void s_entry(void);
extern void strap_entry(void);
extern void m_trap_entry(void);
extern char m_estack_top[];
extern trapframe_t sched_tf;
extern int g_use_sstc;

static unsigned long csr_read_menvcfg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, menvcfg" : "=r"(v));
    return v;
}

static void csr_set_menvcfg(unsigned long mask) {
    __asm__ volatile("csrs menvcfg, %0" :: "r"(mask));
}

void m_boot(void) {
    unsigned long v;

    // Probe for Sstc: try to set menvcfg.STCE and see if the bit sticks.
    // If the hart does not implement Sstc, the bit reads back zero and the
    // S-mode scheduler falls back to an M-mode ecall for timer rearms.
    csr_set_menvcfg(MENVCFG_STCE);
    g_use_sstc = (csr_read_menvcfg() & MENVCFG_STCE) != 0;

    // Park both timer comparators far in the future so no stale pending
    // interrupt fires when S-mode enables its interrupt. mtimecmp is
    // M-mode-only; stimecmp is written only if Sstc is present.
    *(volatile unsigned long *)CLINT_MTIMECMP = ~0UL;
    if (g_use_sstc)
        __asm__ volatile("csrw 0x14d, %0" :: "r"(~0UL));

    // PMP: with no PMP entry programmed, S-mode has no access to any
    // address (M-mode keeps full access, but lower modes default-deny).
    // Open the whole address space to S-mode with one NAPOT entry,
    // R/W/X, before the drop. Without this, the first S-mode instruction
    // fetch raises an instruction access fault.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // mcounteren: the time/cycle/instret CSRs are unreadable in S-mode
    // unless M-mode grants access; at reset all bits are clear, so the
    // trap entry's rdtime/rdcycle would raise illegal instruction (which
    // is delegated back to S-mode, re-entering the trap vector forever).
    __asm__ volatile("csrw mcounteren, %0" :: "r"(0x7UL));  // CY|TM|IR

    // Route supervisor traps and interrupts to S-mode.
    __asm__ volatile("csrw medeleg, %0" :: "r"(MEDELEG_MASK));
    __asm__ volatile("csrw mideleg, %0" :: "r"(MIDELEG_MASK));

    // M-mode keeps a minimal vector: the ecall rearm service only.
    __asm__ volatile("csrw mtvec, %0" :: "r"(m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(m_estack_top));

    // S-mode trap state: scheduler vector, and sscratch pointing at the
    // boot trapframe so the first tick saves main's context there.
    __asm__ volatile("csrw stvec, %0" :: "r"(strap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"(&sched_tf));

    // sret restores privilege from sstatus.SPP, so set SPP=1 (S-mode).
    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    v |= (1UL << 8);  // SPP
    __asm__ volatile("csrw sstatus, %0" :: "r"(v));

    // mstatus.MPP=01 is the M-mode view, kept consistent for the sret.
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    v = (v & ~(3UL << 11)) | (1UL << 11);
    __asm__ volatile("csrw mstatus, %0" :: "r"(v));

    // Drop to S-mode at s_entry. Never returns.
    __asm__ volatile("la t0, s_entry\n"
                     "csrw sepc, t0\n"
                     "sret");
    for (;;)
        __asm__ volatile("wfi");
}

#else  // M-mode baseline: no delegation, stay in M-mode.

void m_boot(void) {
}

#endif
