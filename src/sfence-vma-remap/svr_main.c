// svr_main.c: sfence.vma remap visibility (backlog item
// "riscv sfence-vma-remap").
//
// Exactly one mechanism is under test: what a PTE change does to
// already-cached translations. The RISC-V privileged spec says a
// PTE write is invisible to the hart until an sfence.vma orders it,
// so a remap that skips the fence must read stale and a remap
// followed by the fence must read fresh. The module proves this on
// QEMU 8.2.2 by remapping one virtual address between two physical
// pages holding distinct canaries and reading the word back in
// three S-mode phases:
//
//   Phase A: the PTE maps TEST_VA to PA1 (canary A). Read the word:
//     expect canary A. The read populates the hart's translation
//     cache with the TEST_VA -> PA1 mapping.
//   M-mode rewrites the leaf PTE to point at PA2 (canary B) and
//     issues NO fence.
//   Phase B: read the word again: expect stale canary A, because
//     the cached translation still points at PA1.
//   M-mode issues sfence.vma.
//   Phase C: read the word again: expect fresh canary B.
//
// M-mode builds a minimal Sv39 table by hand: root[2] -> l1_id[0] ->
// l0_id[] identity-maps the program window [0x80000000, 0x80080000)
// with R|W|X, and root[1] -> l1_test[0] -> l0_test[0] is the single
// leaf under test, mapping TEST_VA (VPN2=1, VPN1=0, VPN0=0) to the
// current backing page. One PMP NAPOT entry grants S-mode R|W|X
// over the whole address space (unlocked entries are never checked
// for M-mode, so the M-mode phases are unaffected), and Sv39 is
// enabled via satp + sfence.vma before the drop to S-mode. M-mode
// is never translated, so the M-mode phases, the trap handlers,
// and the save areas all sit inside the identity-mapped window.
// The A/D bits are pre-set on every leaf so no access-fault trap
// fires to update them (menvcfg.ADUE is 0 on this QEMU).
//
// Sequence:
//   M-mode: install the trap handlers, record the boot CSRs, open
//   the address space with PMP, clear mie and mstatus.MIE, plant
//   the canaries, build the page tables, enable Sv39, record the
//   initial PTE, arm the mid_to_B continuation in m_regs[7], then
//   drop to S-mode.
//   S-mode phase A: load the word at TEST_VA (expect canary A),
//   record the ecall site, ecall back to M-mode (cause 9 is not
//   delegated, so it lands in the M-mode handler).
//   M-mode mid_to_B: snapshot the phase-A trap record, rewrite the
//   leaf PTE to PA2 (no fence), record the rewritten PTE, arm
//   mid_to_C, drop to S-mode.
//   S-mode phase B: load the word at TEST_VA (expect stale canary
//   A), record the ecall site, ecall back.
//   M-mode mid_to_C: snapshot the phase-B trap record, execute
//   sfence.vma, arm report_mmode, drop to S-mode.
//   S-mode phase C: load the word at TEST_VA (expect fresh canary
//   B), record the ecall site, ecall back.
//   M-mode report: snapshot the phase-C trap record, restore satp
//   to Bare and the PMP to boot, print every measured value, run
//   the checks, take a quiet window, and print the verdict. All
//   interrupt enables stay clear for the whole run, so no
//   interrupt of either kind can fire.

#include "../uart.h"

#define CAUSE_ECALL_FROM_S 9UL

// The virtual address under test: VPN2=1, VPN1=0, VPN0=0.
#define TEST_VA 0x40000000UL
#define TEST_VPN2 1

// Identity-mapped window: 128 pages, [0x80000000, 0x80080000).
#define MAP_PAGES 128
#define MAP_BASE 0x80000000UL

#define CANARY_A 0xAAAAAAAAAAAAAAAAUL
#define CANARY_B 0xBBBBBBBBBBBBBBBBUL

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
static unsigned long l1_test[512] __attribute__((aligned(4096)));
static unsigned long l0_test[512] __attribute__((aligned(4096)));

// The two backing pages. Each holds its canary at offset 0.
static unsigned long scratch1[512] __attribute__((aligned(4096)));
static unsigned long scratch2[512] __attribute__((aligned(4096)));

static volatile unsigned long m_regs[10];  // mscratch points here
static volatile unsigned long s_regs[10];  // sscratch points here

static unsigned long boot_satp;
static unsigned long boot_medeleg;
static unsigned long satp_rb;
static unsigned long pte_before;  // leaf PTE as built (PA1)
static unsigned long pte_after;   // leaf PTE after the rewrite (PA2)
static unsigned long pA_rb;       // phase-A word readback
static unsigned long pB_rb;       // phase-B word readback (stale expected)
static unsigned long pC_rb;       // phase-C word readback (fresh expected)
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

// S-mode payload shape: load the word at TEST_VA, store the
// readback and the ecall site, then ecall back to M-mode (cause 9,
// not delegated). The ecall site is taken with an in-asm numeric
// local label resolved by the assembler, so -O2 block merging
// cannot move it.
#define PHASE_BODY(rb_var, epc_var)                                     \
    __asm__ volatile(                                                   \
        "li t0, %2\n"          /* t0 = TEST_VA */                        \
        "ld t0, 0(t0)\n"       /* t0 = *TEST_VA */                      \
        "sd t0, 0(%0)\n"       /* rb_var = t0 */                        \
        "la t0, 1f\n"                                                   \
        "sd t0, 0(%1)\n"       /* epc_var = ecall address */            \
        "1:\n"                                                          \
        "ecall\n"                                                       \
        :: "r"(&(rb_var)), "r"(&(epc_var)), "i"(TEST_VA)                \
        : "t0", "memory");                                              \
    for (;;)                                                            \
        __asm__ volatile("wfi");

void smode_phaseA(void) { PHASE_BODY(pA_rb, pA_ecall_pc); }
void smode_phaseB(void) { PHASE_BODY(pB_rb, pB_ecall_pc); }
void smode_phaseC(void) { PHASE_BODY(pC_rb, pC_ecall_pc); }

// M-mode mid phase, entered by the M-mode trap handler redirecting
// mepc here after the phase-A ecall. Snapshots the phase-A trap
// record, rewrites the leaf PTE to point at PA2 (deliberately with
// NO fence), records the rewritten PTE, arms mid_to_C, and drops
// to S-mode for phase B.
void mid_to_B(void) {
    unsigned long expect;

    pA_mcause = m_regs[2];
    pA_mepc = m_regs[3];

    l0_test[0] = (((unsigned long)scratch2 >> 12) << 10) | PTE_LEAF_RW;
    pte_after = l0_test[0];
    expect = (((unsigned long)scratch2 >> 12) << 10) | PTE_LEAF_RW;
    check(pte_after == expect, "PTE rewrite did not land in memory");

    m_regs[7] = (unsigned long)mid_to_C;
    drop_to_smode(smode_phaseB);
}

// M-mode mid phase, entered by the M-mode trap handler redirecting
// mepc here after the phase-B ecall. Snapshots the phase-B trap
// record, issues the sfence.vma that makes the PTE rewrite
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
    unsigned long h, expect_a, expect_b;

    pC_mcause = m_regs[2];
    pC_mepc = m_regs[3];

    // Restore translation off and the PMP to boot (both were 0).
    set_satp(0);
    satp_restored = csr_read_satp();
    __asm__ volatile("csrw pmpcfg0, %0\n"
                     "csrw pmpaddr0, %0" :: "r"(0UL));
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(pmp_restored));

    expect_a = (((unsigned long)scratch1 >> 12) << 10) | PTE_LEAF_RW;
    expect_b = (((unsigned long)scratch2 >> 12) << 10) | PTE_LEAF_RW;

    uart_puts("sfence-vma-remap: PTE remap visibility without/with sfence.vma\n");
    uart_puts("boot: satp=");
    uart_put_hex(boot_satp);
    uart_puts(" medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts("\n");
    uart_puts("table: root[2]->l1[0]->l0 identity [0x80000000,0x80080000) R|W|X; "
              "root[1]->l1[0]->l0[0] leaf maps 0x40000000\n");
    uart_puts("satp=");
    uart_put_hex(satp_rb);
    uart_puts(" (MODE=8 ASID=0)\n");
    uart_puts("pte_before=");
    uart_put_hex(pte_before);
    uart_puts(" (PA1 leaf)\n");
    uart_puts("pte_after=");
    uart_put_hex(pte_after);
    uart_puts(" (PA2 leaf, no fence)\n");

    uart_puts("phaseA: readback=");
    uart_put_hex(pA_rb);
    uart_puts(" expected=");
    uart_put_hex(CANARY_A);
    uart_puts(" mcause=");
    uart_put_hex(pA_mcause);
    uart_puts(" mepc=");
    uart_put_hex(pA_mepc);
    uart_puts(" expected=");
    uart_put_hex(pA_ecall_pc);
    uart_puts("\n");
    uart_puts("phaseB: readback=");
    uart_put_hex(pB_rb);
    uart_puts(" (stale A expected, no fence issued) mcause=");
    uart_put_hex(pB_mcause);
    uart_puts(" mepc=");
    uart_put_hex(pB_mepc);
    uart_puts(" expected=");
    uart_put_hex(pB_ecall_pc);
    uart_puts("\n");
    uart_puts("phaseC: readback=");
    uart_put_hex(pC_rb);
    uart_puts(" (fresh B expected, after sfence.vma) mcause=");
    uart_put_hex(pC_mcause);
    uart_puts(" mepc=");
    uart_put_hex(pC_mepc);
    uart_puts(" expected=");
    uart_put_hex(pC_ecall_pc);
    uart_puts("\n");
    uart_puts("canaries: scratch1[0]=");
    uart_put_hex(scratch1[0]);
    uart_puts(" scratch2[0]=");
    uart_put_hex(scratch2[0]);
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
    check(pte_before == expect_a, "pte_before != PA1 leaf encoding");
    check(pte_after == expect_b, "pte_after != PA2 leaf encoding");

    // The claim under test: without the fence the hart reads the
    // stale mapping, with the fence it reads the fresh one.
    check(pA_rb == CANARY_A, "phase-A readback != canary A");
    check(pB_rb == CANARY_A, "phase-B readback is not the stale canary A");
    check(pB_rb != CANARY_B, "phase-B saw the rewrite without a fence");
    check(pC_rb == CANARY_B, "phase-C readback != canary B after sfence.vma");
    check(scratch1[0] == CANARY_A, "canary A corrupted");
    check(scratch2[0] == CANARY_B, "canary B corrupted");

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
        unsigned long vals[12] = {
            pA_rb, pB_rb, pC_rb, pte_before, pte_after,
            m_regs[0], pA_mcause, pB_mcause, pC_mcause,
            s_regs[0], satp_rb, pA_mepc
        };
        unsigned long i, b;
        for (i = 0; i < 12; i++)
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
    unsigned long root_ppn, l1_ppn, l0_ppn, t1_ppn, t0_ppn, rb;
    int i;

    uart_init();
    uart_puts("sfence-vma-remap: sfence.vma remap visibility test\n");

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
    check(scratch1[0] == CANARY_A, "canary A plant failed");
    check(scratch2[0] == CANARY_B, "canary B plant failed");

    // Build the tables by hand. root[2] is a V-only pointer to the
    // level-1 identity table, l1_id[0] a V-only pointer to the
    // level-0 table, and l0_id[i] identity-maps pages
    // [0x80000000, 0x80080000) with R|W|X. root[1] is a V-only
    // pointer to the test tables, and l0_test[0] is the leaf under
    // test, mapping TEST_VA to PA1. Every other entry in all five
    // tables is zero.
    check(((unsigned long)root_pt & 0xFFFUL) == 0, "root_pt misaligned");
    check(((unsigned long)l1_test & 0xFFFUL) == 0, "l1_test misaligned");
    check(((unsigned long)l0_test & 0xFFFUL) == 0, "l0_test misaligned");
    check(((unsigned long)scratch1 & 0xFFFUL) == 0, "scratch1 misaligned");
    check(((unsigned long)scratch2 & 0xFFFUL) == 0, "scratch2 misaligned");
    root_ppn = (unsigned long)root_pt >> 12;
    l1_ppn = (unsigned long)l1_id >> 12;
    l0_ppn = (unsigned long)l0_id >> 12;
    t1_ppn = (unsigned long)l1_test >> 12;
    t0_ppn = (unsigned long)l0_test >> 12;
    root_pt[TEST_VPN2] = (t1_ppn << 10) | PTE_V;
    l1_test[0] = (t0_ppn << 10) | PTE_V;
    l0_test[0] = (((unsigned long)scratch1 >> 12) << 10) | PTE_LEAF_RW;
    root_pt[2] = (l1_ppn << 10) | PTE_V;
    l1_id[0] = (l0_ppn << 10) | PTE_V;
    for (i = 0; i < MAP_PAGES; i++)
        l0_id[i] = ((((unsigned long)MAP_BASE >> 12) + (unsigned long)i) << 10) |
                   (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);

    check((root_pt[TEST_VPN2] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "root entry 1 is not a pure pointer (V only)");
    check(((root_pt[TEST_VPN2] >> 10) & 0xFFFFFFFFFFFUL) == t1_ppn,
          "root[1] PPN != l1_test page number");
    check((l1_test[0] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "l1_test entry 0 is not a pure pointer (V only)");
    check(((l1_test[0] >> 10) & 0xFFFFFFFFFFFUL) == t0_ppn,
          "l1_test[0] PPN != l0_test page number");
    check((l0_test[0] & (PTE_V | PTE_R | PTE_W)) == (PTE_V | PTE_R | PTE_W),
          "test leaf missing V/R/W");
    check(((l0_test[0] >> 10) & 0xFFFFFFFFFFFUL) ==
          ((unsigned long)scratch1 >> 12),
          "test leaf PPN != scratch1 page number");
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

    pte_before = l0_test[0];
    uart_puts("m-mode: initial leaf PTE=");
    uart_put_hex(pte_before);
    uart_puts(" (maps TEST_VA to PA1)\n");

    // Arm the mid_to_B continuation the M-mode handler jumps to
    // after recording the phase-A ecall, then drop to S-mode.
    m_regs[7] = (unsigned long)mid_to_B;
    uart_puts("m-mode: entering S-mode (phase A, PTE -> PA1)\n");
    drop_to_smode(smode_phaseA);
}
