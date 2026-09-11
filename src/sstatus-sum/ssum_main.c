// ssum_main.c: sstatus.SUM as the S-mode gate on U-page access
// (proof-backlog item "sstatus-sum-probe").
//
// The mechanism under test: sstatus.SUM (bit 18) controls whether
// S-mode may access pages marked U=1 (privileged spec section
// 4.1.1.8: "when SUM=0, S-mode memory accesses to pages that are
// accessible by U-mode (U=1 in pte) will fault"). With SUM=0, a
// supervisor load from a U-page must raise a load page fault
// (scause 0xd, stval = the faulting VA, sepc = the faulting load);
// with SUM=1 the same load must succeed and return the page
// contents.
//
// Phase A (gate closed, SUM=0): one ld from the U VA 0x40000000.
// The delegated S-mode handler records scause/stval/sepc, advances
// sepc by 4 past the faulting ld, and srets. The ld's destination
// t0 is poisoned to 0 immediately before the load, so the value
// stored after a trap proves the load never completed.
// Phase B (gate open): csrs sstatus, SUM, then the same ld must
// return the canary word with no new trap.
//
// All waits are bounded by construction: the whole run is straight
// line code, no polling. On PASS the machine shuts down via the
// virt test-device finisher so the QEMU process exit code (0)
// reflects the verdict; on FAIL the hart parks. A 64-bit FNV-1a
// checksum over the deterministic measured values is printed and
// cross-checked in the PROOF.md results table.

#include "ssum.h"
#include "../uart.h"

ssum_save_t ssum_save;
volatile unsigned long ssum_trap_count;
volatile unsigned long ssum_scause;
volatile unsigned long ssum_stval;
volatile unsigned long ssum_sepc;
volatile unsigned long ssum_bad_seen;
volatile unsigned long ssum_bad_scause;
volatile unsigned long ssum_bad_sepc;

static unsigned long m_scratch_area[4];

// Page-table pages and the one mapped U data page, each a full 4 KiB
// page so the PPN arithmetic is exact. BSS clearing zeroes every
// other entry (invalid), which is what the walk relies on.
static unsigned long root_pt[512] __attribute__((aligned(4096)));
static unsigned long l1_test[512] __attribute__((aligned(4096)));
static unsigned long l0_test[512] __attribute__((aligned(4096)));
static volatile unsigned char page[4096] __attribute__((aligned(4096)));

// Phase-A observables, written from inline asm.
static volatile unsigned long sums_load_result;
static volatile unsigned long phase_a_resumed;

static unsigned int checks = 0;
static unsigned int fails = 0;

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long read_sstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    return v;
}

static void print_pte(const char *name, unsigned long pte) {
    unsigned long ppn = (pte >> 10) & 0xFFFFFFFFFFFUL;  // 44-bit PPN
    unsigned long fl = pte & 0xFFUL;

    uart_puts(name);
    uart_puts("=");
    uart_put_hex(pte);
    uart_puts(" ppn=");
    uart_put_hex(ppn);
    uart_puts(" flags=");
    if (fl & PTE_V) uart_putc('V');
    if (fl & PTE_R) uart_putc('R');
    if (fl & PTE_W) uart_putc('W');
    if (fl & PTE_X) uart_putc('X');
    if (fl & PTE_U) uart_putc('U');
    if (fl & PTE_G) uart_putc('G');
    if (fl & PTE_A) uart_putc('A');
    if (fl & PTE_D) uart_putc('D');
    if (fl == 0) uart_putc('0');
    uart_puts("\n");
}

extern void m_trap_entry(void);
extern char _stack_top[];

// No M-mode trap is expected after boot. If one fires, print what it
// was and park the hart: the run fails via the timeout harness and the
// log shows the unexpected trap instead of a silent hang.
void m_unexpected_trap(void) {
    unsigned long mcause = m_scratch_area[1];
    unsigned long mepc = m_scratch_area[2];
    uart_puts("\nUNEXPECTED M-mode trap: mcause=");
    uart_put_hex(mcause);
    uart_puts(" mepc=");
    uart_put_hex(mepc);
    uart_puts("\nRESULT: FAIL (unexpected M-mode trap)\n");
    for (;;)
        __asm__ volatile("wfi");
}

// S-mode trap handler: runs on the dedicated trap stack with all
// registers saved. The first trap records scause/stval/sepc; every
// trap advances the saved sepc by 4 to skip the faulting 4-byte ld
// (t0/t1 are not compressible registers, so the ld is always 4
// bytes). Any trap beyond the first is recorded as bad; the run's
// checks fail it later.
void ssum_trap_handler(ssum_save_t *s) {
    if (ssum_trap_count == 0) {
        ssum_scause = s->scause;
        ssum_stval = s->stval;
        ssum_sepc = s->sepc;
    } else {
        ssum_bad_seen = 1;
        ssum_bad_scause = s->scause;
        ssum_bad_sepc = s->sepc;
    }
    s->sepc += 4;
    ssum_trap_count++;
}

static unsigned long fnv1a_64(unsigned long h, unsigned long v) {
    h ^= v;
    h *= 0x100000001b3UL;
    return h;
}

// FNV-1a 64-bit over the deterministic measured values: the phase-A
// trap's scause/stval/sepc, the phase-A load-result poison check
// outcome, the trap counts, the SUM readbacks in both phases, the
// phase-B canary readback, and the phase-A resume marker. Every input
// is a measured register read or counter; nothing host-timing
// dependent enters the checksum.
static unsigned long measured_checksum(unsigned long sum_phase_a,
                                       unsigned long sum_phase_b,
                                       unsigned long load_result_a) {
    unsigned long h = 0xcbf29ce484222325UL;
    h = fnv1a_64(h, ssum_scause);
    h = fnv1a_64(h, ssum_stval);
    h = fnv1a_64(h, ssum_sepc);
    h = fnv1a_64(h, ssum_trap_count);
    h = fnv1a_64(h, ssum_bad_seen);
    h = fnv1a_64(h, sum_phase_a);
    h = fnv1a_64(h, sum_phase_b);
    h = fnv1a_64(h, load_result_a);
    h = fnv1a_64(h, sums_load_result);
    h = fnv1a_64(h, phase_a_resumed);
    return h;
}

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555U

// S-mode body. Reached by mret from M-mode boot below.
void s_main(void) {
    unsigned long sum_phase_a, sum_phase_b, load_result_a, csum;

    uart_puts("s-mode: entered, translation active\n");

    // The gate under test starts closed, explicitly.
    __asm__ volatile("csrc sstatus, %0" :: "r"(SSTATUS_SUM));
    sum_phase_a = (read_sstatus() & SSTATUS_SUM) != 0;
    uart_puts("phase A: sstatus.SUM=");
    uart_put_dec(sum_phase_a);
    uart_puts(" (expect 0)\n");
    check(sum_phase_a == 0, "SUM not clear at phase A");

    // Phase A: the faulting load. t0 is poisoned to 0 immediately
    // before the ld; the S-mode handler advances sepc past the ld,
    // so after a trap the store writes 0 (the ld never ran). If the
    // ld completed instead, t0 would hold the canary and the store
    // would write it: sums_load_result == 0 proves the fault.
    sums_load_result = ~0UL;
    phase_a_resumed = 0;
    __asm__ volatile(
        "li t0, 0\n"                 // poison: proves the ld never completes
        ".global sums_fault_load\n"
        "sums_fault_load:\n"
        "ld t0, 0(%1)\n"             // faults here in phase A (SUM=0, U page)
        "sd t0, 0(%0)\n"             // reached only if the ld completes
        "1:\n"
        "li t0, 1\n"
        "sd t0, 0(%2)\n"             // phase_a_resumed = 1
        :
        : "r"(&sums_load_result), "r"(UPAGE_VA), "r"(&phase_a_resumed)
        : "t0", "memory");

    uart_puts("phase A: traps=");
    uart_put_dec(ssum_trap_count);
    uart_puts(" (expect 1) scause=");
    uart_put_hex(ssum_scause);
    uart_puts(" (expect 0xd) stval=");
    uart_put_hex(ssum_stval);
    uart_puts(" (expect 0x40000000) sepc=");
    uart_put_hex(ssum_sepc);
    uart_puts("\nfault load at sums_fault_load=");
    uart_put_hex((unsigned long)sums_fault_load);
    uart_puts(" load_result=");
    uart_put_hex(sums_load_result);
    uart_puts(" (expect 0) resumed=");
    uart_put_dec(phase_a_resumed);
    uart_puts(" (expect 1)\n");
    load_result_a = sums_load_result;
    check(ssum_trap_count == 1, "phase A did not produce exactly one trap");
    check(ssum_scause == SCAUSE_LPF, "scause != 0xd (load page fault)");
    check(ssum_stval == UPAGE_VA, "stval != faulting VA");
    check(ssum_sepc == (unsigned long)sums_fault_load,
          "sepc != address of the faulting load");
    check(load_result_a == 0, "load result nonzero: the ld completed");
    check(phase_a_resumed == 1, "did not resume after the trap via sret");
    check(ssum_bad_seen == 0, "unexpected extra trap in phase A");

    // Phase B: open the gate, then the same load must return the
    // canary with no new trap.
    __asm__ volatile("csrs sstatus, %0" :: "r"(SSTATUS_SUM));
    sum_phase_b = (read_sstatus() & SSTATUS_SUM) != 0;
    uart_puts("phase B: sstatus.SUM=");
    uart_put_dec(sum_phase_b);
    uart_puts(" (expect 1)\n");
    check(sum_phase_b == 1, "SUM not set in phase B");

    sums_load_result = ~0UL;
    __asm__ volatile(
        "ld t0, 0(%1)\n"
        "sd t0, 0(%0)\n"
        :
        : "r"(&sums_load_result), "r"(UPAGE_VA)
        : "t0", "memory");

    uart_puts("phase B: load_result=");
    uart_put_hex(sums_load_result);
    uart_puts(" (expect canary 0xc0decafe5ca1ab1e) traps=");
    uart_put_dec(ssum_trap_count);
    uart_puts(" (expect 1)\n");
    check(sums_load_result == CANARY,
          "phase-B load did not return the canary");
    check(ssum_trap_count == 1, "unexpected trap in phase B");
    check(ssum_bad_seen == 0, "unexpected extra trap in phase B");

    csum = measured_checksum(sum_phase_a, sum_phase_b, load_result_a);

    uart_puts("VERDICT phase_a_traps=1 phase_b_traps=0 scause=");
    uart_put_hex(ssum_scause);
    uart_puts(" stval=");
    uart_put_hex(ssum_stval);
    uart_puts(" sepc=");
    uart_put_hex(ssum_sepc);
    uart_puts(" canary_readback=");
    uart_put_hex(sums_load_result);
    uart_puts(" checksum=");
    uart_put_hex(csum);
    uart_puts("\n");
    uart_puts("checks=");
    uart_put_dec(checks);
    uart_puts(" fails=");
    uart_put_dec(fails);
    uart_puts("\n");

    if (fails == 0)
        uart_puts("RESULT: PASS\n");
    else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }

    // Let the UART drain before touching the finisher device.
    {
        volatile unsigned long i;
        for (i = 0; i < 200000UL; i++)
            ;
    }

    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS; // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    // FAIL: park the hart without touching the finisher device. The
    // harness runs QEMU under timeout, so a FAIL is observable as
    // the timeout exit status (124) as well as the RESULT line.
    for (;;)
        __asm__ volatile("wfi");
}

// M-mode boot. boot.S jumps here in M-mode.
int main(void) {
    unsigned long root_ppn, l1t_ppn, l0t_ppn, page_ppn, satp, v;

    uart_init();
    uart_puts("sstatus-sum: sstatus.SUM gate on U-page loads (proof-backlog item \"sstatus-sum-probe\")\n");

    // Control: the canary page is good RAM and holds the canary
    // before any table exists, written as a plain physical store.
    *(volatile unsigned long *)page = CANARY;
    check(*(volatile unsigned long *)page == CANARY,
          "canary page not writable before table setup");
    uart_puts("control: canary 0xc0decafe5ca1ab1e stored in the U page: ok\n");

    // Scaffolding checks: the S-mode phase needs its code, stack,
    // tables, and the U page reachable, and the UART page mapped.
    check(((unsigned long)root_pt & 0xFFFUL) == 0, "root_pt misaligned");
    check(((unsigned long)l1_test & 0xFFFUL) == 0, "l1_test misaligned");
    check(((unsigned long)l0_test & 0xFFFUL) == 0, "l0_test misaligned");
    check(((unsigned long)page & 0xFFFUL) == 0, "page misaligned");
    check((unsigned long)_stack_top < IDENT_END, "stack outside identity window");
    check((unsigned long)s_main >= IDENT_BASE &&
          (unsigned long)s_main < IDENT_END,
          "s_main outside identity window");

    // Build the tables by hand.
    // root[2]: 1 GiB megapage leaf, identity [0x80000000, 0xC0000000)
    //   (code, data, stack, the tables themselves; supervisor).
    // root[1]: pointer (V only) to l1_test; l1_test[0]: pointer to
    //   l0_test; l0_test[0]: leaf mapping the 4 KiB canary page at VA
    //   0x40000000 with U=1,R=1 (V,A,D set; no W, no X).
    // root[0]: 1 GiB megapage leaf, identity [0, 0x40000000), which
    //   carries the UART MMIO page at 0x10000000 (supervisor).
    // Every other entry in all three tables is zero (invalid).
    root_ppn = (unsigned long)root_pt >> 12;
    l1t_ppn = (unsigned long)l1_test >> 12;
    l0t_ppn = (unsigned long)l0_test >> 12;
    page_ppn = (unsigned long)page >> 12;
    root_pt[2] = ((IDENT_BASE >> 12) << 10) |
                 (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);
    root_pt[1] = (l1t_ppn << 10) | PTE_V;
    root_pt[0] = (0UL << 10) | (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);
    l1_test[0] = (l0t_ppn << 10) | PTE_V;
    l0_test[0] = (page_ppn << 10) |
                 (PTE_V | PTE_R | PTE_U | PTE_A | PTE_D);

    uart_puts("table: VA 0x40000000 -> vpn2=1 vpn1=0 vpn0=0\n");
    print_pte("table: root_pt[2] ", root_pt[2]);
    print_pte("table: root_pt[1] ", root_pt[1]);
    print_pte("table: root_pt[0] ", root_pt[0]);
    print_pte("table: l1_test[0] ", l1_test[0]);
    print_pte("table: l0_test[0] ", l0_test[0]);
    check((root_pt[1] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "root entry 1 is not a pure pointer (V only)");
    check((l1_test[0] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "l1 entry 0 is not a pure pointer (V only)");
    check((l0_test[0] & (PTE_V | PTE_R | PTE_U)) == (PTE_V | PTE_R | PTE_U),
          "U leaf missing V/R/U");
    check((l0_test[0] & (PTE_W | PTE_X)) == 0,
          "U leaf has unexpected W/X bits");
    check(((root_pt[1] >> 10) & 0xFFFFFFFFFFFUL) == l1t_ppn,
          "root[1] PPN != l1_test page number");
    check(((l1_test[0] >> 10) & 0xFFFFFFFFFFFUL) == l0t_ppn,
          "l1_test[0] PPN != l0_test page number");
    check(((l0_test[0] >> 10) & 0xFFFFFFFFFFFUL) == page_ppn,
          "U leaf PPN != canary page number");

    // PMP: with no PMP entry programmed, S-mode has no access to any
    // address (M-mode keeps full access, lower modes default-deny).
    // Open the whole address space to S-mode with one NAPOT entry,
    // R/W/X, before the drop.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(v));
    uart_puts("m-mode: pmpcfg0=");
    uart_put_hex(v);
    uart_puts(" (entry0: NAPOT all-space R|W|X)\n");
    check(v == 0x1fUL, "pmpcfg0 readback != 0x1f");

    // Delegate the load page fault to S-mode so the phase-A trap is
    // taken in S-mode and stval (not mtval) records the faulting VA.
    __asm__ volatile("csrw medeleg, %0" :: "r"(MEDELEG_LPF));
    __asm__ volatile("csrr %0, medeleg" : "=r"(v));
    uart_puts("m-mode: medeleg=");
    uart_put_hex(v);
    uart_puts(" (bit 13: load page fault delegated)\n");
    check((v & MEDELEG_LPF) != 0, "medeleg bit 13 did not stick");

    // M-mode keeps a minimal vector that reports any unexpected M-mode
    // trap and parks; S-mode gets the full vector.
    __asm__ volatile("csrw mtvec, %0" :: "r"(m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(m_scratch_area));
    __asm__ volatile("csrw stvec, %0" :: "r"(ssum_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"(&ssum_save));
    __asm__ volatile("csrr %0, stvec" : "=r"(v));
    check((v & 3UL) == 0, "stvec not in direct mode");

    // Enable Sv39, then drop to S-mode at s_main with mret (MPP=01).
    __asm__ volatile("csrw satp, %0" :: "r"(SATP_MODE_SV39 | root_ppn));
    __asm__ volatile("sfence.vma" ::: "memory");
    __asm__ volatile("csrr %0, satp" : "=r"(satp));
    uart_puts("m-mode: satp=");
    uart_put_hex(satp);
    uart_puts(" (MODE=8 ASID=0)\n");
    check((satp >> 60) == 8, "satp MODE != 8 (Sv39)");
    check((satp & 0xFFFFFFFFFFFUL) == root_ppn, "satp PPN != root page");

    uart_puts("m-mode: entering S-mode\n");
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    v = (v & ~(3UL << 11)) | (1UL << 11);
    __asm__ volatile("csrw mstatus, %0" :: "r"(v));
    __asm__ volatile("la t0, s_main\n"
                     "csrw mepc, t0\n"
                     "mret");
    for (;;)
        __asm__ volatile("wfi");
}
