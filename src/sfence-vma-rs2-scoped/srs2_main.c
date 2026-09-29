// srs2_main.c: sfence.vma rs2-scoped (ASID-scoped) invalidation
// (backlog item "riscv sfence-vma-rs2-asid-scoped").
//
// Exactly one mechanism is under test: the rs2 operand of
// sfence.vma scopes the invalidation to the named ASID. The
// RISC-V privileged spec says that when rs1 is x0 and rs2 is
// not x0, the fence applies only to address translations for
// the ASID in rs2, so a same-VA remap fenced for ASID 1 alone
// must leave ASID 2's cached translation stale. The module
// measures what QEMU 8.2.2 actually does. M-mode builds two Sv39
// roots by hand: root1 (ASID 1) maps one virtual address VA to
// the canary-A page, root2 (ASID 2) maps the SAME VA to the
// canary-B page. Then:
//
//   Phase A (satp = root1/ASID1): read VA, expect canary A.
//     The read populates the hart's translation cache for the
//     (ASID 1, VA) entry.
//   Phase B (satp = root2/ASID2): read VA, expect canary B.
//     The read populates the cache for the (ASID 2, VA) entry.
//   M-mode rewrites both leaf PTEs (root1 leaf -> canary-C page,
//     root2 leaf -> canary-D page) with deliberately NO fence and
//     no satp write, so satp still selects root2/ASID2.
//   Phase C (still ASID2): read VA, expect stale canary B. This
//     is the cached-stale baseline: the leaf rewrite alone
//     invalidates nothing.
//   M-mode issues sfence.vma with rs1 = x0 and rs2 = 1 (ASID 1
//     only). The disassembly confirms the encoding (rs1 is x0,
//     the rs2 operand register holds 1).
//   Phase D (still ASID2): read VA and record. Correct ASID
//     scoping reads stale canary B; an over-invalidating fence
//     reads fresh canary D.
//   M-mode reinstalls root1/ASID1 with a full fence.
//   Phase E (ASID1): read VA, expect fresh canary C.
//
// Working expectation, measured 2026-09-28 by the sibling module
// src/sfence-vma-rs1-scoped: on QEMU 8.2.2 any sfence.vma
// behaves as a full local TLB flush, so phase D is expected to
// read fresh canary D. The module asserts what the hart does,
// measured first: if phase D had read stale B, the shipped
// assertion would name correct scoping instead. Phases C and D
// deliberately read under ASID2 with satp untouched since phase
// B, so the stale baseline in C and the phase-D verdict are
// attributable to the fence alone, never to a satp write.
//
// M-mode builds both roots by hand: root1[1] -> l1a[0] -> l0a[0]
// is the leaf under test for ASID 1 (VPN2=1), root2[1] -> l1b[0]
// -> l0b[0] is the leaf under test for ASID 2, and both
// root[2] -> l1_id[0] -> l0_id[] identity-map the program window
// [0x80000000, 0x80080000) with R|W|X. One PMP NAPOT entry grants
// S-mode R|W|X over the whole address space; Sv39 is enabled via
// satp + sfence.vma before the drop to S-mode. The A/D bits are
// pre-set on every leaf so no access-fault trap fires to update
// them (menvcfg.ADUE is 0 on this QEMU). All interrupt enables
// stay clear for the whole run, so no interrupt of either kind
// can fire.
//
// Sequence:
//   M-mode: install the trap handlers, record the boot CSRs, open
//   the address space with PMP, clear mie and mstatus.MIE, plant
//   the four canaries, build both page tables, enable Sv39 with
//   root1/ASID1, record the initial PTEs, arm the mid_to_B
//   continuation in m_regs[7], then drop to S-mode.
//   S-mode phase A: load the word at TEST_VA (expect canary A),
//   record the ecall site, ecall back to M-mode (cause 9 is not
//   delegated, so it lands in the M-mode handler).
//   M-mode mid_to_B: snapshot the phase-A trap record, install
//   root2/ASID2 with a full fence, arm mid_to_C, drop to S-mode.
//   S-mode phase B: load the word at TEST_VA (expect canary B),
//   record the ecall site, ecall back.
//   M-mode mid_to_C: snapshot the phase-B trap record, rewrite
//   both leaf PTEs (no fence, no satp write), record the
//   rewritten PTEs, arm mid_to_D, drop to S-mode.
//   S-mode phase C: load the word at TEST_VA (expect stale
//   canary B: the cached-stale baseline), record the ecall site,
//   ecall back.
//   M-mode mid_to_D: snapshot the phase-C trap record, issue
//   sfence.vma with rs1 = x0 and rs2 = 1 (ASID 1 only), arm
//   mid_to_E, drop to S-mode.
//   S-mode phase D: load the word at TEST_VA and record it (the
//   scoping verdict), record the ecall site, ecall back.
//   M-mode mid_to_E: snapshot the phase-D trap record, install
//   root1/ASID1 with a full fence, arm report_mmode, drop to
//   S-mode.
//   S-mode phase E: load the word at TEST_VA (expect fresh canary
//   C), record the ecall site, ecall back.
//   M-mode report: snapshot the phase-E trap record, restore satp
//   to Bare and the PMP entry to boot, print every measured
//   value, run the checks, take a quiet window, and print the
//   verdict. Runs in M-mode, which is never translated, so the
//   UART and the save areas are plain physical accesses.

#include "../uart.h"

#define CAUSE_ECALL_FROM_S 9UL

// The virtual address under test: VPN2=1, same VA under both ASIDs.
#define TEST_VA 0x40000000UL
#define TEST_VPN2 1

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
#define SATP_ASID1 (1UL << 44)
#define SATP_ASID2 (2UL << 44)
#define SATP_ASID_MASK (0xFFFUL << 44)
#define SATP_PPN_MASK 0xFFFFFFFFFFFUL

// Page-table pages, each a full 4 KiB page so the PPN arithmetic is
// exact. BSS clearing zeroes every other entry (invalid).
static unsigned long root1[512] __attribute__((aligned(4096)));
static unsigned long l1a[512] __attribute__((aligned(4096)));
static unsigned long l0a[512] __attribute__((aligned(4096)));
static unsigned long root2[512] __attribute__((aligned(4096)));
static unsigned long l1b[512] __attribute__((aligned(4096)));
static unsigned long l0b[512] __attribute__((aligned(4096)));
static unsigned long l1_id[512] __attribute__((aligned(4096)));
static unsigned long l0_id[512] __attribute__((aligned(4096)));

// The four backing pages. Each holds its canary at offset 0; the
// last word of each stays zero as a no-side-effect sentinel.
static unsigned long scratch1[512] __attribute__((aligned(4096)));
static unsigned long scratch2[512] __attribute__((aligned(4096)));
static unsigned long scratch3[512] __attribute__((aligned(4096)));
static unsigned long scratch4[512] __attribute__((aligned(4096)));

static volatile unsigned long m_regs[10];  // mscratch points here
static volatile unsigned long s_regs[10];  // sscratch points here

static unsigned long boot_satp;
static unsigned long boot_medeleg;
static unsigned long satpA_rb;   // phase-A satp (root1/ASID1)
static unsigned long satpB_rb;   // phase-B satp (root2/ASID2)
static unsigned long satpE_rb;   // phase-E satp (root1/ASID1)
static unsigned long pte1_before;  // root1 leaf as built (VA -> PA1)
static unsigned long pte1_after;   // root1 leaf after rewrite (VA -> PA3)
static unsigned long pte2_before;  // root2 leaf as built (VA -> PA2)
static unsigned long pte2_after;   // root2 leaf after rewrite (VA -> PA4)
static unsigned long pA, pB, pC, pD, pE;  // the five readbacks
static unsigned long pA_epc, pB_epc, pC_epc, pD_epc, pE_epc;
static unsigned long pA_mcause, pA_mepc;
static unsigned long pB_mcause, pB_mepc;
static unsigned long pC_mcause, pC_mepc;
static unsigned long pD_mcause, pD_mepc;
static unsigned long pE_mcause, pE_mepc;
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
void smode_phaseD(void);
void smode_phaseE(void);
void mid_to_B(void);
void mid_to_C(void);
void mid_to_D(void);
void mid_to_E(void);
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

// S-mode payload shape: load the word at TEST_VA, store the
// readback and the ecall site, then ecall back to M-mode (cause
// 9, not delegated). The ecall site is taken with an in-asm
// numeric local label resolved by the assembler, so -O2 block
// merging cannot move it. Any faulting load lands in the S-mode
// trap entry, which parks the hart, so a fault can never be
// mistaken for a measured readback.
#define PHASE_BODY(rb_var, epc_var)                                \
    __asm__ volatile(                                              \
        "li t0, %2\n"          /* t0 = TEST_VA */                   \
        "ld t0, 0(t0)\n"       /* t0 = *TEST_VA */                  \
        "sd t0, 0(%0)\n"       /* rb_var = t0 */                    \
        "la t0, 1f\n"                                               \
        "sd t0, 0(%1)\n"       /* epc_var = ecall address */         \
        "1:\n"                                                      \
        "ecall\n"                                                   \
        :: "r"(&(rb_var)), "r"(&(epc_var)), "i"(TEST_VA)            \
        : "t0", "memory");                                         \
    for (;;)                                                       \
        __asm__ volatile("wfi");

void smode_phaseA(void) { PHASE_BODY(pA, pA_epc); }
void smode_phaseB(void) { PHASE_BODY(pB, pB_epc); }
void smode_phaseC(void) { PHASE_BODY(pC, pC_epc); }
void smode_phaseD(void) { PHASE_BODY(pD, pD_epc); }
void smode_phaseE(void) { PHASE_BODY(pE, pE_epc); }

// M-mode mid phase, entered by the M-mode trap handler redirecting
// mepc here after the phase-A ecall. Snapshots the phase-A trap
// record, installs root2/ASID2 with a full fence, arms mid_to_C,
// and drops to S-mode for phase B.
void mid_to_B(void) {
    unsigned long root2_ppn;

    pA_mcause = m_regs[2];
    pA_mepc = m_regs[3];

    root2_ppn = (unsigned long)root2 >> 12;
    set_satp(SATP_MODE_SV39 | SATP_ASID2 | root2_ppn);
    satpB_rb = csr_read_satp();

    m_regs[7] = (unsigned long)mid_to_C;
    drop_to_smode(smode_phaseB);
}

// M-mode mid phase, entered by the M-mode trap handler redirecting
// mepc here after the phase-B ecall. Snapshots the phase-B trap
// record, rewrites both leaf PTEs (root1 leaf -> PA3, root2 leaf
// -> PA4) with deliberately NO fence and NO satp write, so satp
// still selects root2/ASID2 for phase C. Records the rewritten
// PTEs, arms mid_to_D, and drops to S-mode.
void mid_to_C(void) {
    unsigned long expect1, expect2;

    pB_mcause = m_regs[2];
    pB_mepc = m_regs[3];

    l0a[0] = (((unsigned long)scratch3 >> 12) << 10) | PTE_LEAF_RW;
    l0b[0] = (((unsigned long)scratch4 >> 12) << 10) | PTE_LEAF_RW;
    pte1_after = l0a[0];
    pte2_after = l0b[0];
    expect1 = (((unsigned long)scratch3 >> 12) << 10) | PTE_LEAF_RW;
    expect2 = (((unsigned long)scratch4 >> 12) << 10) | PTE_LEAF_RW;
    check(pte1_after == expect1, "root1 PTE rewrite did not land in memory");
    check(pte2_after == expect2, "root2 PTE rewrite did not land in memory");

    m_regs[7] = (unsigned long)mid_to_D;
    drop_to_smode(smode_phaseC);
}

// M-mode mid phase, entered by the M-mode trap handler redirecting
// mepc here after the phase-C ecall. Snapshots the phase-C trap
// record, then issues the one fence this module is about:
// sfence.vma with rs1 = x0 and rs2 = 1 (ASID 1 only).
// Architecturally this invalidates only ASID 1's translations, so
// ASID 2's cached (VA -> PA2) entry must survive it; the phase-D
// readback records what the hart actually did. Arms mid_to_E and
// drops to S-mode.
void mid_to_D(void) {
    pC_mcause = m_regs[2];
    pC_mepc = m_regs[3];

    // The scoped fence: rs1 = x0, rs2 operand holds 1 (ASID 1).
    // The disassembly shows the li of 1 immediately before
    // "sfence.vma zero, <reg>", i.e. x0 in the rs1 field and the
    // ASID value 1 in the rs2 operand.
    __asm__ volatile("sfence.vma zero, %0" :: "r"(1UL) : "memory");

    m_regs[7] = (unsigned long)mid_to_E;
    drop_to_smode(smode_phaseD);
}

// M-mode mid phase, entered by the M-mode trap handler redirecting
// mepc here after the phase-D ecall. Snapshots the phase-D trap
// record, reinstalls root1/ASID1 with a full fence (so phase E
// reads the remapped leaf), arms report_mmode, and drops to
// S-mode.
void mid_to_E(void) {
    unsigned long root1_ppn;

    pD_mcause = m_regs[2];
    pD_mepc = m_regs[3];

    root1_ppn = (unsigned long)root1 >> 12;
    set_satp(SATP_MODE_SV39 | SATP_ASID1 | root1_ppn);
    satpE_rb = csr_read_satp();

    m_regs[7] = (unsigned long)report_mmode;
    drop_to_smode(smode_phaseE);
}

// M-mode report, entered by the M-mode trap handler redirecting
// mepc here after the phase-E ecall. Snapshots the phase-E trap
// record, restores satp to Bare and the PMP entry to boot, prints
// every measured value, runs the checks, takes a quiet window, and
// prints the verdict. Runs in M-mode, which is never translated,
// so the UART and the save areas are plain physical accesses.
void report_mmode(void) {
    unsigned long h, e1a, e1b, e2a, e2b;
    unsigned long root1_ppn, root2_ppn;

    pE_mcause = m_regs[2];
    pE_mepc = m_regs[3];

    // Restore translation off and the PMP to boot (both were 0).
    set_satp(0);
    satp_restored = csr_read_satp();
    __asm__ volatile("csrw pmpcfg0, %0\n"
                     "csrw pmpaddr0, %0" :: "r"(0UL));
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(pmp_restored));

    root1_ppn = (unsigned long)root1 >> 12;
    root2_ppn = (unsigned long)root2 >> 12;
    e1a = (((unsigned long)scratch1 >> 12) << 10) | PTE_LEAF_RW;
    e1b = (((unsigned long)scratch3 >> 12) << 10) | PTE_LEAF_RW;
    e2a = (((unsigned long)scratch2 >> 12) << 10) | PTE_LEAF_RW;
    e2b = (((unsigned long)scratch4 >> 12) << 10) | PTE_LEAF_RW;

    uart_puts("sfence-vma-rs2-scoped: sfence.vma rs2-scoped (ASID) invalidation\n");
    uart_puts("boot: satp=");
    uart_put_hex(boot_satp);
    uart_puts(" medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts("\n");
    uart_puts("table: root1[1]->l1[0]->l0[0] leaf maps 0x40000000 (ASID 1); "
              "root2[1]->l1[0]->l0[0] leaf maps 0x40000000 (ASID 2); "
              "root[2]->l1[0]->l0[] identity [0x80000000,0x80080000) R|W|X\n");
    uart_puts("satpA=");
    uart_put_hex(satpA_rb);
    uart_puts(" (MODE=8 ASID=1) satpB=");
    uart_put_hex(satpB_rb);
    uart_puts(" (MODE=8 ASID=2) satpE=");
    uart_put_hex(satpE_rb);
    uart_puts(" (MODE=8 ASID=1)\n");
    uart_puts("pte1_before=");
    uart_put_hex(pte1_before);
    uart_puts(" (ASID1 leaf -> PA1) pte2_before=");
    uart_put_hex(pte2_before);
    uart_puts(" (ASID2 leaf -> PA2)\n");
    uart_puts("pte1_after=");
    uart_put_hex(pte1_after);
    uart_puts(" (ASID1 leaf -> PA3) pte2_after=");
    uart_put_hex(pte2_after);
    uart_puts(" (ASID2 leaf -> PA4, no fence, no satp write)\n");

    uart_puts("phaseA: va=");
    uart_put_hex(pA);
    uart_puts(" (A expected) mcause=");
    uart_put_hex(pA_mcause);
    uart_puts(" mepc=");
    uart_put_hex(pA_mepc);
    uart_puts(" expected=");
    uart_put_hex(pA_epc);
    uart_puts("\n");
    uart_puts("phaseB: va=");
    uart_put_hex(pB);
    uart_puts(" (B expected) mcause=");
    uart_put_hex(pB_mcause);
    uart_puts(" mepc=");
    uart_put_hex(pB_mepc);
    uart_puts(" expected=");
    uart_put_hex(pB_epc);
    uart_puts("\n");
    uart_puts("phaseC: va=");
    uart_put_hex(pC);
    uart_puts(" (stale B expected: no fence yet) mcause=");
    uart_put_hex(pC_mcause);
    uart_puts(" mepc=");
    uart_put_hex(pC_mepc);
    uart_puts(" expected=");
    uart_put_hex(pC_epc);
    uart_puts("\n");
    uart_puts("phaseD: va=");
    uart_put_hex(pD);
    uart_puts(" (after sfence.vma rs1=x0,rs2=1) mcause=");
    uart_put_hex(pD_mcause);
    uart_puts(" mepc=");
    uart_put_hex(pD_mepc);
    uart_puts(" expected=");
    uart_put_hex(pD_epc);
    uart_puts("\n");
    uart_puts("phaseE: va=");
    uart_put_hex(pE);
    uart_puts(" (fresh C expected, ASID1 reinstalled+fenced) mcause=");
    uart_put_hex(pE_mcause);
    uart_puts(" mepc=");
    uart_put_hex(pE_mepc);
    uart_puts(" expected=");
    uart_put_hex(pE_epc);
    uart_puts("\n");
    uart_puts("canaries: s1=");
    uart_put_hex(scratch1[0]);
    uart_puts(" s2=");
    uart_put_hex(scratch2[0]);
    uart_puts(" s3=");
    uart_put_hex(scratch3[0]);
    uart_puts(" s4=");
    uart_put_hex(scratch4[0]);
    uart_puts(" sentinels: s1e=");
    uart_put_hex(scratch1[511]);
    uart_puts(" s2e=");
    uart_put_hex(scratch2[511]);
    uart_puts(" s3e=");
    uart_put_hex(scratch3[511]);
    uart_puts(" s4e=");
    uart_put_hex(scratch4[511]);
    uart_puts("\n");
    uart_puts("traps: m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts("\n");

    // Translation setup checks.
    check((satpA_rb >> 60) == 8, "satpA MODE != 8 (Sv39)");
    check(((satpA_rb >> 44) & 0xFFFUL) == 1, "satpA ASID != 1");
    check((satpA_rb & SATP_PPN_MASK) == root1_ppn, "satpA PPN != root1 page");
    check((satpB_rb >> 60) == 8, "satpB MODE != 8 (Sv39)");
    check(((satpB_rb >> 44) & 0xFFFUL) == 2, "satpB ASID != 2");
    check((satpB_rb & SATP_PPN_MASK) == root2_ppn, "satpB PPN != root2 page");
    check((satpE_rb >> 60) == 8, "satpE MODE != 8 (Sv39)");
    check(((satpE_rb >> 44) & 0xFFFUL) == 1, "satpE ASID != 1");
    check((satpE_rb & SATP_PPN_MASK) == root1_ppn, "satpE PPN != root1 page");
    check(pte1_before == e1a, "pte1_before != ASID1->PA1 leaf encoding");
    check(pte2_before == e2a, "pte2_before != ASID2->PA2 leaf encoding");
    check(pte1_after == e1b, "pte1_after != ASID1->PA3 leaf encoding");
    check(pte2_after == e2b, "pte2_after != ASID2->PA4 leaf encoding");

    // Phase A/B: each ASID reads its own mapping, populating the
    // cache for its (ASID, VA) entry.
    check(pA == CANARY_A, "phase-A ASID1 readback != canary A");
    check(pA != CANARY_B && pA != CANARY_C && pA != CANARY_D,
          "phase-A readback is not cleanly canary A");
    check(pB == CANARY_B, "phase-B ASID2 readback != canary B");
    check(pB != CANARY_A && pB != CANARY_C && pB != CANARY_D,
          "phase-B readback is not cleanly canary B");

    // Phase C: the cached-stale baseline. Both leaves were
    // rewritten with no fence and no satp write, and satp still
    // selects root2/ASID2, so the phase-B translation must still
    // be cached: the read must return stale canary B.
    check(pC == CANARY_B, "phase-C readback != stale canary B (baseline broken)");
    check(pC != CANARY_D, "phase-C unexpectedly fresh (rewrite invalidated it?)");

    // The claim under test, as measured: the sfence.vma with
    // rs1 = x0 and rs2 = 1 did NOT scope the invalidation to
    // ASID 1. Phase D runs under ASID2 with satp untouched since
    // phase B, so a correctly scoped fence would leave the
    // cached (ASID 2, VA) entry stale (canary B); the hart
    // instead reads fresh canary D, i.e. the fence
    // over-invalidated, behaving as a full local flush. This
    // matches the sibling module src/sfence-vma-rs1-scoped, which
    // measured the same full-flush behavior for the rs1 operand,
    // and the Grinch OS observation that QEMU treats any
    // sfence.vma as a full local TLB flush (lfd/grinch commit
    // ccccd68). Over-invalidation is still correct, just not
    // ASID-scoped.
    check(pD == CANARY_D, "phase-D ASID2 did not go fresh after the rs2=1 fence");
    check(pD != CANARY_B, "phase-D ASID2 unexpectedly stale (fence scoped on this run?)");

    // Phase E: ASID1 was reinstalled with a full fence, so the
    // remapped leaf must read fresh canary C.
    check(pE == CANARY_C, "phase-E ASID1 readback != fresh canary C");
    check(pE != CANARY_A, "phase-E ASID1 still shows the stale mapping");

    // Canary integrity and the no-side-effect sentinels.
    check(scratch1[0] == CANARY_A, "canary A corrupted");
    check(scratch2[0] == CANARY_B, "canary B corrupted");
    check(scratch3[0] == CANARY_C, "canary C corrupted");
    check(scratch4[0] == CANARY_D, "canary D corrupted");
    check(scratch1[511] == 0, "scratch1 sentinel moved (side effect)");
    check(scratch2[511] == 0, "scratch2 sentinel moved (side effect)");
    check(scratch3[511] == 0, "scratch3 sentinel moved (side effect)");
    check(scratch4[511] == 0, "scratch4 sentinel moved (side effect)");

    // Control flow checks: exactly five M-mode ecalls, no S-mode
    // trap of any kind, and each mepc at its phase's ecall site.
    check(s_regs[0] == 0, "S-mode trap fired (expected 0)");
    check(m_regs[0] == 5, "M-mode trap count != 5 (the five ecalls)");
    check(pA_mcause == CAUSE_ECALL_FROM_S &&
          pB_mcause == CAUSE_ECALL_FROM_S &&
          pC_mcause == CAUSE_ECALL_FROM_S &&
          pD_mcause == CAUSE_ECALL_FROM_S &&
          pE_mcause == CAUSE_ECALL_FROM_S,
          "an M-mode trap was not the expected S-mode ecall");
    check(pA_mepc == pA_epc, "phase-A mepc != its ecall site");
    check(pB_mepc == pB_epc, "phase-B mepc != its ecall site");
    check(pC_mepc == pC_epc, "phase-C mepc != its ecall site");
    check(pD_mepc == pD_epc, "phase-D mepc != its ecall site");
    check(pE_mepc == pE_epc, "phase-E mepc != its ecall site");

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
    check(m_regs[0] == 5 && s_regs[0] == 0,
          "trap count moved during the quiet window");

    // FNV-1a over the verdict values, so the three runs can be
    // compared byte for byte.
    h = 0xcbf29ce484222325UL;
    {
        unsigned long vals[13] = {
            pA, pB, pC, pD, pE,
            pte1_before, pte2_before, pte1_after, pte2_after,
            m_regs[0], s_regs[0], satpA_rb, satpB_rb
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
    unsigned long root1_ppn, a1_ppn, a0_ppn, b1_ppn, b0_ppn, l1_ppn, l0_ppn, rb;
    int i;

    uart_init();
    uart_puts("sfence-vma-rs2-scoped: sfence.vma rs2-scoped (ASID) invalidation test\n");

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
    // in M-mode. The only traps in this run are the five phase
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

    // Build both tables by hand. root1[TEST_VPN2] is a V-only
    // pointer to l1a, l1a[0] a V-only pointer to l0a, and l0a[0]
    // is the leaf under test for ASID 1, mapping TEST_VA to PA1.
    // root2[TEST_VPN2], l1b, l0b mirror this for ASID 2, mapping
    // the same TEST_VA to PA2. Both roots' entry 2 is a V-only
    // pointer to the shared identity tables l1_id -> l0_id[],
    // which map [0x80000000, 0x80080000) with R|W|X. Every other
    // entry in all eight tables is zero.
    check(((unsigned long)root1 & 0xFFFUL) == 0, "root1 misaligned");
    check(((unsigned long)l1a & 0xFFFUL) == 0, "l1a misaligned");
    check(((unsigned long)l0a & 0xFFFUL) == 0, "l0a misaligned");
    check(((unsigned long)root2 & 0xFFFUL) == 0, "root2 misaligned");
    check(((unsigned long)l1b & 0xFFFUL) == 0, "l1b misaligned");
    check(((unsigned long)l0b & 0xFFFUL) == 0, "l0b misaligned");
    check(((unsigned long)l1_id & 0xFFFUL) == 0, "l1_id misaligned");
    check(((unsigned long)l0_id & 0xFFFUL) == 0, "l0_id misaligned");
    check(((unsigned long)scratch1 & 0xFFFUL) == 0, "scratch1 misaligned");
    check(((unsigned long)scratch2 & 0xFFFUL) == 0, "scratch2 misaligned");
    check(((unsigned long)scratch3 & 0xFFFUL) == 0, "scratch3 misaligned");
    check(((unsigned long)scratch4 & 0xFFFUL) == 0, "scratch4 misaligned");
    root1_ppn = (unsigned long)root1 >> 12;
    a1_ppn = (unsigned long)l1a >> 12;
    a0_ppn = (unsigned long)l0a >> 12;
    b1_ppn = (unsigned long)l1b >> 12;
    b0_ppn = (unsigned long)l0b >> 12;
    l1_ppn = (unsigned long)l1_id >> 12;
    l0_ppn = (unsigned long)l0_id >> 12;
    root1[TEST_VPN2] = (a1_ppn << 10) | PTE_V;
    l1a[0] = (a0_ppn << 10) | PTE_V;
    l0a[0] = (((unsigned long)scratch1 >> 12) << 10) | PTE_LEAF_RW;
    root2[TEST_VPN2] = (b1_ppn << 10) | PTE_V;
    l1b[0] = (b0_ppn << 10) | PTE_V;
    l0b[0] = (((unsigned long)scratch2 >> 12) << 10) | PTE_LEAF_RW;
    root1[2] = (l1_ppn << 10) | PTE_V;
    root2[2] = (l1_ppn << 10) | PTE_V;
    l1_id[0] = (l0_ppn << 10) | PTE_V;
    for (i = 0; i < MAP_PAGES; i++)
        l0_id[i] = ((((unsigned long)MAP_BASE >> 12) + (unsigned long)i) << 10) |
                   (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);

    check((root1[TEST_VPN2] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "root1 test entry is not a pure pointer (V only)");
    check(((root1[TEST_VPN2] >> 10) & SATP_PPN_MASK) == a1_ppn,
          "root1 test entry PPN != l1a page number");
    check((l1a[0] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "l1a entry 0 is not a pure pointer (V only)");
    check(((l1a[0] >> 10) & SATP_PPN_MASK) == a0_ppn,
          "l1a[0] PPN != l0a page number");
    check((l0a[0] & (PTE_V | PTE_R | PTE_W)) == (PTE_V | PTE_R | PTE_W),
          "ASID1 test leaf missing V/R/W");
    check(((l0a[0] >> 10) & SATP_PPN_MASK) == ((unsigned long)scratch1 >> 12),
          "ASID1 test leaf PPN != scratch1 page number");
    check((root2[TEST_VPN2] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "root2 test entry is not a pure pointer (V only)");
    check(((root2[TEST_VPN2] >> 10) & SATP_PPN_MASK) == b1_ppn,
          "root2 test entry PPN != l1b page number");
    check((l1b[0] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "l1b entry 0 is not a pure pointer (V only)");
    check(((l1b[0] >> 10) & SATP_PPN_MASK) == b0_ppn,
          "l1b[0] PPN != l0b page number");
    check((l0b[0] & (PTE_V | PTE_R | PTE_W)) == (PTE_V | PTE_R | PTE_W),
          "ASID2 test leaf missing V/R/W");
    check(((l0b[0] >> 10) & SATP_PPN_MASK) == ((unsigned long)scratch2 >> 12),
          "ASID2 test leaf PPN != scratch2 page number");
    check((root1[2] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "root1 identity entry is not a pure pointer (V only)");
    check((root2[2] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "root2 identity entry is not a pure pointer (V only)");
    check((unsigned long)smode_phaseA < MAP_BASE + (MAP_PAGES << 12),
          "smode_phaseA outside identity window");
    check((unsigned long)smode_phaseB < MAP_BASE + (MAP_PAGES << 12),
          "smode_phaseB outside identity window");
    check((unsigned long)smode_phaseC < MAP_BASE + (MAP_PAGES << 12),
          "smode_phaseC outside identity window");
    check((unsigned long)smode_phaseD < MAP_BASE + (MAP_PAGES << 12),
          "smode_phaseD outside identity window");
    check((unsigned long)smode_phaseE < MAP_BASE + (MAP_PAGES << 12),
          "smode_phaseE outside identity window");
    check((unsigned long)s_trap_entry < MAP_BASE + (MAP_PAGES << 12),
          "s_trap_entry outside identity window");
    check((unsigned long)&s_regs[9] < MAP_BASE + (MAP_PAGES << 12),
          "s_regs outside identity window");

    // Enable Sv39 with root1/ASID1, then confirm the mode, ASID,
    // and root stuck.
    set_satp(SATP_MODE_SV39 | SATP_ASID1 | root1_ppn);
    satpA_rb = csr_read_satp();
    uart_puts("m-mode: satp=");
    uart_put_hex(satpA_rb);
    uart_puts(" (MODE=8 ASID=1)\n");

    pte1_before = l0a[0];
    pte2_before = l0b[0];
    uart_puts("m-mode: initial leaf PTEs: ASID1=");
    uart_put_hex(pte1_before);
    uart_puts(" ASID2=");
    uart_put_hex(pte2_before);
    uart_puts("\n");

    // Arm the mid_to_B continuation the M-mode handler jumps to
    // after recording the phase-A ecall, then drop to S-mode.
    m_regs[7] = (unsigned long)mid_to_B;
    uart_puts("m-mode: entering S-mode (phase A, ASID1: VA->PA1)\n");
    drop_to_smode(smode_phaseA);
}
