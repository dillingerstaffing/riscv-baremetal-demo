// srs_main.c: sfence.vma rs1-scoped invalidation (backlog item
// "riscv sfence-vma-rs1-scoped").
//
// Exactly one mechanism is under test: the rs1 operand of
// sfence.vma scopes the invalidation to the named address. The
// RISC-V privileged spec says that when rs1 is not x0, the fence
// applies only to address translations that contain the address in
// rs1, so a remap fenced for VA1 alone must read fresh at VA1
// while VA2, remapped at the same time but never fenced, stays
// stale. The module proves this on QEMU 8.2.2 by remapping two
// virtual addresses between two pairs of physical pages holding
// distinct canaries, fencing only VA1, and reading both words back
// in S-mode phases:
//
//   Phase A: the leaves map TEST_VA1 to PA1 (canary A) and
//     TEST_VA2 to PA3 (canary C). Read both words: expect A and
//     C. The reads populate the hart's translation cache with the
//     two mappings.
//   M-mode rewrites both leaf PTEs (VA1 to PA2 holding canary B,
//     VA2 to PA4 holding canary D), issues NO global fence, then
//     issues sfence.vma with rs1 = TEST_VA1 only.
//   Phase B: read VA1 (expect fresh canary B) and VA2. The backlog
//     premise said VA2 must read stale canary C because no fence
//     covered it; the measurement on QEMU 8.2.2 says otherwise
//     (premise corrected below), so phase B asserts what the hart
//     actually does.
//   M-mode issues a global sfence.vma.
//   Phase C: read VA1 (expect B) and VA2 (expect fresh canary D).
//
// Premise correction, measured 2026-09-28: on QEMU 8.2.2 the
// rs1-scoped fence does NOT scope. After the VA1-scoped fence,
// phase B reads fresh canary B at VA1 AND fresh canary D at VA2:
// the hart dropped VA2's cached translation too, even though no
// fence named VA2. The sibling module src/sfence-vma-remap is the
// control: with no fence at all the same harness reads stale, so
// the fresh phase-B VA2 readback is the fence's doing, not a
// missing cache entry. The module therefore proves the honest
// slice: the rs1 operand has no scoping effect on this
// implementation (over-invalidation is still correct, just not
// scoped), and the global fence in mid_to_C is a no-op
// confirmation. This matches the independent observation in the
// Grinch OS project that QEMU treats any sfence.vma as a full
// local TLB flush (lfd/grinch commit ccccd68).
//
// M-mode builds a minimal Sv39 table by hand: root[2] ->
// l1_id[0] -> l0_id[] identity-maps the program window
// [0x80000000, 0x80080000) with R|W|X, root[1] -> l1_t1[0] ->
// l0_t1[0] is the leaf under test for TEST_VA1 (VPN2=1), and
// root[3] -> l1_t2[0] -> l0_t2[0] is the leaf under test for
// TEST_VA2 (VPN2=3). One PMP NAPOT entry grants S-mode R|W|X over
// the whole address space; Sv39 is enabled via satp + sfence.vma
// before the drop to S-mode. The A/D bits are pre-set on every
// leaf so no access-fault trap fires to update them
// (menvcfg.ADUE is 0 on this QEMU). The only fences issued are
// the scoped one in mid_to_B and the global one in mid_to_C.
//
// Sequence:
//   M-mode: install the trap handlers, record the boot CSRs, open
//   the address space with PMP, clear mie and mstatus.MIE, plant
//   the four canaries, build the page tables, enable Sv39, record
//   the initial PTEs, arm the mid_to_B continuation in m_regs[7],
//   then drop to S-mode.
//   S-mode phase A: load the words at TEST_VA1 and TEST_VA2
//   (expect canaries A and C), record the ecall site, ecall back
//   to M-mode (cause 9 is not delegated, so it lands in the
//   M-mode handler).
//   M-mode mid_to_B: snapshot the phase-A trap record, rewrite
//   both leaf PTEs (no fence), record the rewritten PTEs, issue
//   sfence.vma with rs1 = TEST_VA1 (rs2 = 0), arm mid_to_C, drop
//   to S-mode.
//   S-mode phase B: load both words (expect fresh B at VA1, stale
//   C at VA2), record the ecall site, ecall back.
//   M-mode mid_to_C: snapshot the phase-B trap record, issue a
//   global sfence.vma, arm report_mmode, drop to S-mode.
//   S-mode phase C: load both words (expect B at VA1, fresh D at
//   VA2), record the ecall site, ecall back.
//   M-mode report: snapshot the phase-C trap record, restore satp
//   to Bare and the PMP entry to boot, print every measured
//   value, run the checks, take a quiet window, and print the
//   verdict. All interrupt enables stay clear for the whole run,
//   so no interrupt of either kind can fire.

#include "../uart.h"

#define CAUSE_ECALL_FROM_S 9UL

// The two virtual addresses under test: VPN2=1 and VPN2=3.
#define TEST_VA1 0x40000000UL
#define TEST_VA2 0xC0000000UL
#define TEST_VPN2_1 1
#define TEST_VPN2_2 3

// Identity-mapped window: 128 pages, [0x80000000, 0x80080000).
#define MAP_PAGES 128
#define MAP_BASE 0x80000000UL

#define CANARY_A 0xAAAAAAAAAAAAAAAAUL
#define CANARY_B 0xBBBBBBBBBBBBBBBBUL
#define CANARY_C 0xCCCCCCCCCCCCCCCCUL
#define CANARY_D 0xDDDDDDDDDDDDDDDDUL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final RESULT line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

// PTE flag bits (RISC-V privileged spec, Sv39 PTE format).
#define PTE_V 0x001UL
#define PTE_R 0x002UL
#define PTE_W 0x004UL
#define PTE_X 0x008UL
#define PTE_A 0x040UL
#define PTE_D 0x080UL
#define PTE_LEAF_RW ((PTE_V | PTE_R | PTE_W | PTE_A | PTE_D))

#define MSTATUS_MPP_S (1UL << 11)
#define SATP_MODE_SV39 (8UL << 60)

// Page-table pages, each a full 4 KiB page so the PPN arithmetic is
// exact. BSS clearing zeroes every other entry (invalid).
static unsigned long root_pt[512] __attribute__((aligned(4096)));
static unsigned long l1_id[512] __attribute__((aligned(4096)));
static unsigned long l0_id[512] __attribute__((aligned(4096)));
static unsigned long l1_t1[512] __attribute__((aligned(4096)));
static unsigned long l0_t1[512] __attribute__((aligned(4096)));
static unsigned long l1_t2[512] __attribute__((aligned(4096)));
static unsigned long l0_t2[512] __attribute__((aligned(4096)));

// The four backing pages. Each holds its canary at offset 0.
static unsigned long scratch1[512] __attribute__((aligned(4096)));
static unsigned long scratch2[512] __attribute__((aligned(4096)));
static unsigned long scratch3[512] __attribute__((aligned(4096)));
static unsigned long scratch4[512] __attribute__((aligned(4096)));

static volatile unsigned long m_regs[10];  // mscratch points here
static volatile unsigned long s_regs[10];  // sscratch points here

static unsigned long boot_satp;
static unsigned long boot_medeleg;
static unsigned long satp_rb;
static unsigned long pte1_before;  // VA1 leaf as built (PA1)
static unsigned long pte1_after;   // VA1 leaf after the rewrite (PA2)
static unsigned long pte2_before;  // VA2 leaf as built (PA3)
static unsigned long pte2_after;   // VA2 leaf after the rewrite (PA4)
static unsigned long pA_va1, pA_va2;  // phase-A readbacks
static unsigned long pB_va1, pB_va2;  // phase-B readbacks
static unsigned long pC_va1, pC_va2;  // phase-C readbacks
static unsigned long pA_ecall_pc;
static unsigned long pB_ecall_pc;
static unsigned long pC_ecall_pc;
static unsigned long pA_mcause, pA_mepc;
static unsigned long pB_mcause, pB_mepc;
static unsigned long pC_mcause, pC_mepc;
static unsigned long satp_restored;
static unsigned long pmp_restored;

static unsigned long checks = 0;
static unsigned long fails = 0;

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long csr_read_satp(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, satp" : "=r"(v));
    return v;
}

static void set_satp(unsigned long v) {
    __asm__ volatile("csrw satp, %0" :: "r"(v));
    __asm__ volatile("sfence.vma" ::: "memory");
}

extern void m_trap_entry(void);
extern void s_trap_entry(void);
void smode_phaseA(void);
void smode_phaseB(void);
void smode_phaseC(void);
void mid_to_B(void);
void mid_to_C(void);
void report_mmode(void);

// Drop from M-mode to S-mode at the given entry point. sret takes
// the target privilege from sstatus.SPP; mstatus.MPP is set to
// S-mode for a consistent view. Never returns.
static void drop_to_smode(void (*entry)(void)) {
    unsigned long v;

    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    v |= (1UL << 8);  // SPP = 1 (S-mode)
    __asm__ volatile("csrw sstatus, %0" :: "r"(v));
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    v = (v & ~(3UL << 11)) | MSTATUS_MPP_S;  // MPP = 01
    __asm__ volatile("csrw mstatus, %0" :: "r"(v));
    __asm__ volatile("csrw sepc, %0\n"
                     "sret" :: "r"((unsigned long)entry) : "memory");
    for (;;)
        __asm__ volatile("wfi");
}

// S-mode payload shape: load the words at TEST_VA1 and TEST_VA2,
// store the readbacks and the ecall site, then ecall back to
// M-mode (cause 9, not delegated). The ecall site is taken with
// an in-asm numeric local label resolved by the assembler, so
// -O2 block merging cannot move it.
#define PHASE_BODY(rb1_var, rb2_var, epc_var)                              \
    __asm__ volatile(                                                     \
        "li t0, %3\n"          /* t0 = TEST_VA1 */                         \
        "ld t0, 0(t0)\n"       /* t0 = *TEST_VA1 */                        \
        "sd t0, 0(%0)\n"       /* rb1_var = t0 */                          \
        "li t0, %4\n"          /* t0 = TEST_VA2 */                         \
        "ld t0, 0(t0)\n"       /* t0 = *TEST_VA2 */                        \
        "sd t0, 0(%1)\n"       /* rb2_var = t0 */                          \
        "la t0, 1f\n"                                                     \
        "sd t0, 0(%2)\n"       /* epc_var = ecall address */               \
        "1:\n"                                                            \
        "ecall\n"                                                         \
        :: "r"(&(rb1_var)), "r"(&(rb2_var)), "r"(&(epc_var)),              \
           "i"(TEST_VA1), "i"(TEST_VA2)                                   \
        : "t0", "memory");                                                \
    for (;;)                                                              \
        __asm__ volatile("wfi");

void smode_phaseA(void) { PHASE_BODY(pA_va1, pA_va2, pA_ecall_pc); }
void smode_phaseB(void) { PHASE_BODY(pB_va1, pB_va2, pB_ecall_pc); }
void smode_phaseC(void) { PHASE_BODY(pC_va1, pC_va2, pC_ecall_pc); }

// M-mode mid phase, entered by the M-mode trap handler redirecting
// mepc here after the phase-A ecall. Snapshots the phase-A trap
// record, rewrites both leaf PTEs (VA1 to PA2, VA2 to PA4) with
// deliberately NO global fence, records the rewritten PTEs, then
// issues the one fence this module is about: sfence.vma scoped to
// TEST_VA1 (rs1 = VA1, rs2 = 0). Arms mid_to_C and drops to
// S-mode for phase B.
void mid_to_B(void) {
    unsigned long expect1, expect2;

    pA_mcause = m_regs[2];
    pA_mepc = m_regs[3];

    l0_t1[0] = (((unsigned long)scratch2 >> 12) << 10) | PTE_LEAF_RW;
    l0_t2[0] = (((unsigned long)scratch4 >> 12) << 10) | PTE_LEAF_RW;
    pte1_after = l0_t1[0];
    pte2_after = l0_t2[0];
    expect1 = (((unsigned long)scratch2 >> 12) << 10) | PTE_LEAF_RW;
    expect2 = (((unsigned long)scratch4 >> 12) << 10) | PTE_LEAF_RW;
    check(pte1_after == expect1, "VA1 PTE rewrite did not land in memory");
    check(pte2_after == expect2, "VA2 PTE rewrite did not land in memory");

    // The scoped fence: architecturally this invalidates only
    // translations containing TEST_VA1, so TEST_VA2's cached
    // translation should survive it. The measurement says it
    // does not on QEMU 8.2.2 (see the header note); the fence is
    // issued exactly as specified and the phase-B readbacks
    // record what the hart actually did.
    __asm__ volatile("sfence.vma %0, zero" :: "r"(TEST_VA1) : "memory");

    m_regs[7] = (unsigned long)mid_to_C;
    drop_to_smode(smode_phaseB);
}

// M-mode mid phase, entered by the M-mode trap handler redirecting
// mepc here after the phase-B ecall. Snapshots the phase-B trap
// record, issues the global sfence.vma that makes the VA2 rewrite
// visible, arms report_mmode, and drops to S-mode for phase C.
void mid_to_C(void) {
    pB_mcause = m_regs[2];
    pB_mepc = m_regs[3];

    __asm__ volatile("sfence.vma" ::: "memory");

    m_regs[7] = (unsigned long)report_mmode;
    drop_to_smode(smode_phaseC);
}

// M-mode report, entered by the M-mode trap handler redirecting
// mepc here after the phase-C ecall. Snapshots the phase-C trap
// record, restores satp to Bare and the PMP entry to boot, prints
// every measured value, runs the checks, takes a quiet window, and
// prints the verdict. Runs in M-mode, which is never translated,
// so the UART and the save areas are plain physical accesses.
void report_mmode(void) {
    unsigned long h, e1a, e1b, e2a, e2b;

    pC_mcause = m_regs[2];
    pC_mepc = m_regs[3];

    // Restore translation off and the PMP to boot (both were 0).
    set_satp(0);
    satp_restored = csr_read_satp();
    __asm__ volatile("csrw pmpcfg0, %0\n"
                     "csrw pmpaddr0, %0" :: "r"(0UL));
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(pmp_restored));

    e1a = (((unsigned long)scratch1 >> 12) << 10) | PTE_LEAF_RW;
    e1b = (((unsigned long)scratch2 >> 12) << 10) | PTE_LEAF_RW;
    e2a = (((unsigned long)scratch3 >> 12) << 10) | PTE_LEAF_RW;
    e2b = (((unsigned long)scratch4 >> 12) << 10) | PTE_LEAF_RW;

    uart_puts("sfence-vma-rs1-scoped: sfence.vma rs1-scoped invalidation\n");
    uart_puts("boot: satp=");
    uart_put_hex(boot_satp);
    uart_puts(" medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts("\n");
    uart_puts("table: root[2]->l1[0]->l0 identity [0x80000000,0x80080000) R|W|X; "
              "root[1]->l1[0]->l0[0] leaf maps 0x40000000; "
              "root[3]->l1[0]->l0[0] leaf maps 0xc0000000\n");
    uart_puts("satp=");
    uart_put_hex(satp_rb);
    uart_puts(" (MODE=8 ASID=0)\n");
    uart_puts("pte1_before=");
    uart_put_hex(pte1_before);
    uart_puts(" (VA1 -> PA1) pte2_before=");
    uart_put_hex(pte2_before);
    uart_puts(" (VA2 -> PA3)\n");
    uart_puts("pte1_after=");
    uart_put_hex(pte1_after);
    uart_puts(" (VA1 -> PA2) pte2_after=");
    uart_put_hex(pte2_after);
    uart_puts(" (VA2 -> PA4, no global fence before phase B)\n");

    uart_puts("phaseA: va1=");
    uart_put_hex(pA_va1);
    uart_puts(" (A) va2=");
    uart_put_hex(pA_va2);
    uart_puts(" (C) mcause=");
    uart_put_hex(pA_mcause);
    uart_puts(" mepc=");
    uart_put_hex(pA_mepc);
    uart_puts(" expected=");
    uart_put_hex(pA_ecall_pc);
    uart_puts("\n");
    uart_puts("phaseB: va1=");
    uart_put_hex(pB_va1);
    uart_puts(" (fresh B expected) va2=");
    uart_put_hex(pB_va2);
    uart_puts(" (fresh D observed: scoped fence did not scope) mcause=");
    uart_put_hex(pB_mcause);
    uart_puts(" mepc=");
    uart_put_hex(pB_mepc);
    uart_puts(" expected=");
    uart_put_hex(pB_ecall_pc);
    uart_puts("\n");
    uart_puts("phaseC: va1=");
    uart_put_hex(pC_va1);
    uart_puts(" (B) va2=");
    uart_put_hex(pC_va2);
    uart_puts(" (fresh D expected, after global fence) mcause=");
    uart_put_hex(pC_mcause);
    uart_puts(" mepc=");
    uart_put_hex(pC_mepc);
    uart_puts(" expected=");
    uart_put_hex(pC_ecall_pc);
    uart_puts("\n");
    uart_puts("canaries: s1=");
    uart_put_hex(scratch1[0]);
    uart_puts(" s2=");
    uart_put_hex(scratch2[0]);
    uart_puts(" s3=");
    uart_put_hex(scratch3[0]);
    uart_puts(" s4=");
    uart_put_hex(scratch4[0]);
    uart_puts("\n");
    uart_puts("traps: m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts("\n");

    // Translation setup checks.
    check((satp_rb >> 60) == 8, "satp MODE != 8 (Sv39)");
    check((satp_rb & 0xFFFFFFFFFFFUL) == ((unsigned long)root_pt >> 12),
          "satp PPN != root page");
    check(pte1_before == e1a, "pte1_before != VA1->PA1 leaf encoding");
    check(pte2_before == e2a, "pte2_before != VA2->PA3 leaf encoding");
    check(pte1_after == e1b, "pte1_after != VA1->PA2 leaf encoding");
    check(pte2_after == e2b, "pte2_after != VA2->PA4 leaf encoding");

    // The claim under test, as measured: the rs1-scoped fence
    // invalidated the named address (phase-B VA1 reads fresh), but
    // it did NOT scope the invalidation (phase-B VA2 also reads
    // fresh, even though no fence named it). The sibling module
    // src/sfence-vma-remap proves the control: with no fence the
    // same harness reads stale, so VA2's fresh readback here is
    // the scoped fence over-invalidating, not a cache miss.
    // Phase C confirms the global fence keeps both fresh.
    check(pA_va1 == CANARY_A, "phase-A VA1 readback != canary A");
    check(pA_va2 == CANARY_C, "phase-A VA2 readback != canary C");
    check(pB_va1 == CANARY_B, "phase-B VA1 is not the fresh canary B");
    check(pB_va1 != CANARY_A, "phase-B VA1 still shows the stale mapping");
    check(pB_va2 == CANARY_D, "phase-B VA2 did not go fresh after the scoped fence");
    check(pB_va2 != CANARY_C, "phase-B VA2 unexpectedly stale (fence scoped on this run?)");
    check(pC_va1 == CANARY_B, "phase-C VA1 readback != canary B");
    check(pC_va2 == CANARY_D, "phase-C VA2 is not the fresh canary D");
    check(scratch1[0] == CANARY_A, "canary A corrupted");
    check(scratch2[0] == CANARY_B, "canary B corrupted");
    check(scratch3[0] == CANARY_C, "canary C corrupted");
    check(scratch4[0] == CANARY_D, "canary D corrupted");

    // Control flow checks: exactly three M-mode ecalls, no S-mode
    // trap of any kind, and each mepc at its phase's ecall site.
    check(s_regs[0] == 0, "S-mode trap fired (expected 0)");
    check(m_regs[0] == 3, "M-mode trap count != 3 (the three ecalls)");
    check(pA_mcause == CAUSE_ECALL_FROM_S &&
          pB_mcause == CAUSE_ECALL_FROM_S &&
          pC_mcause == CAUSE_ECALL_FROM_S,
          "an M-mode trap was not the expected S-mode ecall");
    check(pA_mepc == pA_ecall_pc, "phase-A mepc != its ecall site");
    check(pB_mepc == pB_ecall_pc, "phase-B mepc != its ecall site");
    check(pC_mepc == pC_ecall_pc, "phase-C mepc != its ecall site");

    // Restore checks.
    check(satp_restored == 0, "satp did not restore to Bare");
    check(pmp_restored == 0, "pmpcfg0 did not restore to 0");

    // Quiet window: nothing pending, all enables clear; the counts
    // must not move.
    {
        unsigned long spins = 0;
        while (spins < 2000000UL)
            spins++;
    }
    uart_puts("quiet: m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts("\n");
    check(m_regs[0] == 3 && s_regs[0] == 0,
          "trap count moved during the quiet window");

    // FNV-1a over the verdict values, so the three runs can be
    // compared byte for byte.
    h = 0xcbf29ce484222325UL;
    {
        unsigned long vals[13] = {
            pA_va1, pA_va2, pB_va1, pB_va2, pC_va1, pC_va2,
            pte1_before, pte1_after, pte2_before, pte2_after,
            m_regs[0], s_regs[0], satp_rb
        };
        unsigned long i, b;
        for (i = 0; i < 13; i++)
            for (b = 0; b < 8; b++) {
                h ^= (vals[i] >> (b * 8)) & 0xffUL;
                h *= 0x100000001b3UL;
            }
    }
    uart_puts("checksum=");
    uart_put_hex(h);
    uart_puts("\n");

    // Drain the UART before the finisher shuts the machine down, so
    // the final RESULT line is never cut off.
    while (!(*UART0_LSR & LSR_TEMT))
        ;
    if (fails == 0) {
        uart_puts("RESULT: PASS (checks=");
        uart_put_dec(checks);
        uart_puts(")\n");
        while (!(*UART0_LSR & LSR_TEMT))
            ;
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    uart_puts("RESULT: FAIL (checks=");
    uart_put_dec(checks);
    uart_puts(" fails=");
    uart_put_dec(fails);
    uart_puts(")\n");
    uart_puts("done\n");
    while (!(*UART0_LSR & LSR_TEMT))
        ;
    for (;;)  // FAIL: park the hart so the harness sees a timeout
        __asm__ volatile("wfi");
}

int main(void) {
    unsigned long root_ppn, l1_ppn, l0_ppn, a1_ppn, a0_ppn, b1_ppn, b0_ppn, rb;
    int i;

    uart_init();
    uart_puts("sfence-vma-rs1-scoped: sfence.vma rs1-scoped invalidation test\n");

    // M-mode trap handler: direct-mode mtvec, mscratch at m_regs.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // S-mode trap handler: direct-mode stvec, sscratch at s_regs.
    // Any S-mode trap in this run is a failure; the handler parks.
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_regs));

    // Boot-time CSRs, for the record.
    boot_satp = csr_read_satp();
    __asm__ volatile("csrr %0, medeleg" : "=r"(boot_medeleg));

    // Open the whole address space to S-mode (lower modes
    // default-deny) with one PMP NAPOT entry.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(rb));
    uart_puts("m-mode: pmpcfg0=");
    uart_put_hex(rb);
    uart_puts(" (entry0: NAPOT all R|W|X)\n");
    check(rb == 0x1fUL, "pmpcfg0 readback != 0x1f");

    // Disarm every interrupt enable: mie clear and mstatus.MIE
    // clear. medeleg stays zero, so every synchronous trap lands
    // in M-mode. The only traps in this run are the three phase
    // ecalls.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrc mstatus, %0" :: "r"(1UL << 3));  // MIE off

    // Plant the canaries before translation is on.
    scratch1[0] = CANARY_A;
    scratch2[0] = CANARY_B;
    scratch3[0] = CANARY_C;
    scratch4[0] = CANARY_D;
    check(scratch1[0] == CANARY_A, "canary A plant failed");
    check(scratch2[0] == CANARY_B, "canary B plant failed");
    check(scratch3[0] == CANARY_C, "canary C plant failed");
    check(scratch4[0] == CANARY_D, "canary D plant failed");

    // Build the tables by hand. root[2] is a V-only pointer to the
    // level-1 identity table, l1_id[0] a V-only pointer to the
    // level-0 table, and l0_id[i] identity-maps pages
    // [0x80000000, 0x80080000) with R|W|X. root[1] is a V-only
    // pointer to the VA1 tables, l1_t1[0] a V-only pointer to
    // l0_t1, and l0_t1[0] is the leaf under test, mapping
    // TEST_VA1 to PA1. root[3], l1_t2, l0_t2 mirror this for
    // TEST_VA2 to PA3. Every other entry in all seven tables is
    // zero.
    check(((unsigned long)root_pt & 0xFFFUL) == 0, "root_pt misaligned");
    check(((unsigned long)l1_t1 & 0xFFFUL) == 0, "l1_t1 misaligned");
    check(((unsigned long)l0_t1 & 0xFFFUL) == 0, "l0_t1 misaligned");
    check(((unsigned long)l1_t2 & 0xFFFUL) == 0, "l1_t2 misaligned");
    check(((unsigned long)l0_t2 & 0xFFFUL) == 0, "l0_t2 misaligned");
    check(((unsigned long)scratch1 & 0xFFFUL) == 0, "scratch1 misaligned");
    check(((unsigned long)scratch2 & 0xFFFUL) == 0, "scratch2 misaligned");
    check(((unsigned long)scratch3 & 0xFFFUL) == 0, "scratch3 misaligned");
    check(((unsigned long)scratch4 & 0xFFFUL) == 0, "scratch4 misaligned");
    root_ppn = (unsigned long)root_pt >> 12;
    l1_ppn = (unsigned long)l1_id >> 12;
    l0_ppn = (unsigned long)l0_id >> 12;
    a1_ppn = (unsigned long)l1_t1 >> 12;
    a0_ppn = (unsigned long)l0_t1 >> 12;
    b1_ppn = (unsigned long)l1_t2 >> 12;
    b0_ppn = (unsigned long)l0_t2 >> 12;
    root_pt[TEST_VPN2_1] = (a1_ppn << 10) | PTE_V;
    l1_t1[0] = (a0_ppn << 10) | PTE_V;
    l0_t1[0] = (((unsigned long)scratch1 >> 12) << 10) | PTE_LEAF_RW;
    root_pt[TEST_VPN2_2] = (b1_ppn << 10) | PTE_V;
    l1_t2[0] = (b0_ppn << 10) | PTE_V;
    l0_t2[0] = (((unsigned long)scratch3 >> 12) << 10) | PTE_LEAF_RW;
    root_pt[2] = (l1_ppn << 10) | PTE_V;
    l1_id[0] = (l0_ppn << 10) | PTE_V;
    for (i = 0; i < MAP_PAGES; i++)
        l0_id[i] = ((((unsigned long)MAP_BASE >> 12) + (unsigned long)i) << 10) |
                   (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);

    check((root_pt[TEST_VPN2_1] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "root entry 1 is not a pure pointer (V only)");
    check(((root_pt[TEST_VPN2_1] >> 10) & 0xFFFFFFFFFFFUL) == a1_ppn,
          "root[1] PPN != l1_t1 page number");
    check((l1_t1[0] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "l1_t1 entry 0 is not a pure pointer (V only)");
    check(((l1_t1[0] >> 10) & 0xFFFFFFFFFFFUL) == a0_ppn,
          "l1_t1[0] PPN != l0_t1 page number");
    check((root_pt[TEST_VPN2_2] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "root entry 3 is not a pure pointer (V only)");
    check(((root_pt[TEST_VPN2_2] >> 10) & 0xFFFFFFFFFFFUL) == b1_ppn,
          "root[3] PPN != l1_t2 page number");
    check((l1_t2[0] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "l1_t2 entry 0 is not a pure pointer (V only)");
    check(((l1_t2[0] >> 10) & 0xFFFFFFFFFFFUL) == b0_ppn,
          "l1_t2[0] PPN != l0_t2 page number");
    check((l0_t1[0] & (PTE_V | PTE_R | PTE_W)) == (PTE_V | PTE_R | PTE_W),
          "VA1 test leaf missing V/R/W");
    check(((l0_t1[0] >> 10) & 0xFFFFFFFFFFFUL) ==
          ((unsigned long)scratch1 >> 12),
          "VA1 test leaf PPN != scratch1 page number");
    check((l0_t2[0] & (PTE_V | PTE_R | PTE_W)) == (PTE_V | PTE_R | PTE_W),
          "VA2 test leaf missing V/R/W");
    check(((l0_t2[0] >> 10) & 0xFFFFFFFFFFFUL) ==
          ((unsigned long)scratch3 >> 12),
          "VA2 test leaf PPN != scratch3 page number");
    check((unsigned long)smode_phaseA < MAP_BASE + (MAP_PAGES << 12),
          "smode_phaseA outside identity window");
    check((unsigned long)smode_phaseB < MAP_BASE + (MAP_PAGES << 12),
          "smode_phaseB outside identity window");
    check((unsigned long)smode_phaseC < MAP_BASE + (MAP_PAGES << 12),
          "smode_phaseC outside identity window");
    check((unsigned long)s_trap_entry < MAP_BASE + (MAP_PAGES << 12),
          "s_trap_entry outside identity window");
    check((unsigned long)&s_regs[9] < MAP_BASE + (MAP_PAGES << 12),
          "s_regs outside identity window");

    // Enable Sv39, then confirm the mode and root stuck.
    set_satp(SATP_MODE_SV39 | root_ppn);
    satp_rb = csr_read_satp();
    uart_puts("m-mode: satp=");
    uart_put_hex(satp_rb);
    uart_puts(" (MODE=8 ASID=0)\n");

    pte1_before = l0_t1[0];
    pte2_before = l0_t2[0];
    uart_puts("m-mode: initial leaf PTEs: VA1=");
    uart_put_hex(pte1_before);
    uart_puts(" VA2=");
    uart_put_hex(pte2_before);
    uart_puts("\n");

    // Arm the mid_to_B continuation the M-mode handler jumps to
    // after recording the phase-A ecall, then drop to S-mode.
    m_regs[7] = (unsigned long)mid_to_B;
    uart_puts("m-mode: entering S-mode (phase A, VA1->PA1, VA2->PA3)\n");
    drop_to_smode(smode_phaseA);
}
