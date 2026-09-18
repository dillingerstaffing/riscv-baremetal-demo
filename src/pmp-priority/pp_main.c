// pp_main.c: pmp entry-priority first-match test
// (backlog item "riscv pmp-priority-first-match").
//
// Exactly one mechanism is under test: PMP matching is
// priority-ordered, and the lowest-numbered matching entry wins.
// Two PMP entries are programmed to match the SAME 4 KiB scratch
// page with conflicting permissions, and an S-mode load from the
// page is observed as the entries are swapped. Phase A (entry 0 =
// allow, entry 1 = deny): the load completes and returns the
// canary. Phase B (entry 0 = deny, entry 1 = allow): the same load
// traps with mcause = 0x5 (load access fault), mepc at the faulting
// load, mtval at the scratch page.
//
// Why NAPOT, not TOR (backlog correction): the backlog text says
// TOR, but TOR entries are half-open ranges that partition the
// address space, so two TOR entries can never both match one
// address. A true priority test needs two entries that both MATCH
// the same address with conflicting permissions, so both entries
// are NAPOT over the same 4 KiB page with the same pmpaddr value.
// The mechanism under test is entry priority, not address
// matching, which distinguishes this module from the done
// src/pmp-tor (match semantics) and src/pmp-lock-bit (locking).
//
// Locked PMP entries cannot be reprogrammed, so the test entries
// are UNLOCKED; unlocked entries do not apply to M-mode, so the
// probe load runs in S-mode: M-mode drops to S-mode via sret with
// satp staying Bare (no page tables needed), the S-mode payload
// writes a canary to the scratch page and reads it back with lbu,
// then ecall returns to M-mode. medeleg stays 0, so the phase-B
// fault traps in M-mode. With any PMP entry implemented, an
// S-mode access with no matching entry FAILS (default deny), so a
// higher-numbered broad allow entry (entry 2, NAPOT R|W|X over
// [0x80000000, 0x100000000)) keeps S-mode code fetch and the
// control load working outside the scratch page. Entries 0/1 keep
// priority over it for the scratch page.
//
// Phase A: entry 0 = NAPOT R|W allow, entry 1 = NAPOT no-perms
// deny over the same page. The S-mode lbu of the canary completes
// with no trap; the ecall returns to M-mode (cause 9). Phase B:
// pmpcfg0 is rewritten (unlocked, writable) with entry 0 = deny,
// entry 1 = allow. The same S-mode lbu traps in M-mode with
// mcause = 0x5, mepc at the faulting load, mtval at the scratch
// page; the handler records it and redirects to the report.
//
// Controls: each phase first does an lbu of a control byte from a
// word covered ONLY by the broad allow entry; it completes in both
// phases, which proves the phase-B trap comes from the priority
// swap and not from the load path itself. A quiet spin window
// moves neither trap counter, and the boot PMP config is restored
// and reported at the end. All interrupt enables stay clear for
// the whole run, so no interrupt of either kind can fire.
//
// Sequence:
//   M-mode: install the trap handlers, record the boot PMP config
//   and medeleg, compute the NAPOT pmpaddr for the scratch page,
//   program entries 0/1 (same pmpaddr) and entry 2 (broad allow),
//   verify every readback, clear mie and mstatus.MIE, arm the
//   phase-2 continuation in m_regs[7], drop to S-mode.
//   S-mode phase A: write the canary, lbu it back (no trap, entry
//   0 allow wins), lbu the control byte, ecall to M-mode (cause 9,
//   not delegated). The M-mode handler records the ecall and
//   redirects mepc to phase2_mmode.
//   M-mode phase 2: snapshots the phase-A counters, rewrites
//   pmpcfg0 swapped (entry 0 deny, entry 1 allow), verifies the
//   readbacks, arms the report continuation, drops to S-mode.
//   S-mode phase B: lbu the control byte (completes), record the
//   faulting lbu's address, lbu the scratch page. Entry 0 (deny)
//   wins the priority match, so the load traps in M-mode with
//   mcause = 0x5. The handler records mcause/mepc/mstatus/mtval
//   and redirects to report_mmode, which restores the boot PMP
//   config, prints every measured value, runs the checks, takes
//   a quiet window, and prints the verdict.
//
// The fault-site address is taken with `la t1, 0f` (an in-asm
// numeric local label, never C &&label, which the toolchain
// miscompiles at -O2). The M-mode handler never advances mepc
// past the faulting load: it redirects to an armed continuation,
// and the checks assert mepc equals the recorded site, so no
// instruction-length arithmetic exists to get wrong if the load
// were emitted compressed.

#include "../uart.h"

#define CAUSE_LOAD_ACCESS_FAULT 5UL
#define CAUSE_ECALL_FROM_S 9UL

// The 4 KiB scratch page both test entries cover. Its physical
// address is taken at run time (satp stays Bare, so VA == PA).
static unsigned char scratch_page[4096] __attribute__((aligned(4096)));
// Control byte: lives in .data, covered ONLY by the broad allow
// entry 2, never by entries 0/1. Loaded in both phases; it must
// complete both times.
static volatile unsigned char control_byte = 0x5A;

#define PAGE_SIZE 0x1000UL
// NAPOT encoding for a 4 KiB region: pmpaddr = (base >> 2) with the
// low 9 bits set ((size >> 3) - 1 = 0x1FF).
#define NAPOT_4K_MASK 0x1FFUL
// Entry 2: NAPOT over [0x80000000, 0x100000000): base 0x80000000,
// size 0x80000000, so pmpaddr = 0x20000000 | 0x0FFFFFFF.
#define PMPADDR2_BROAD 0x2FFFFFFFUL
#define BROAD_BASE 0x80000000UL
#define BROAD_TOP 0x100000000UL

// pmpcfg0 byte layout: entry i occupies bits [8*i+7, 8*i].
// Phase A: entry0 = 0x1B (NAPOT, R|W), entry1 = 0x18 (NAPOT, no
// perms), entry2 = 0x1F (NAPOT, R|W|X), entry3 = 0x00.
#define PMPCFG0_PHASEA 0x001F181BUL
// Phase B: entry 0 and entry 1 swapped.
#define PMPCFG0_PHASEB 0x001F1B18UL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final RESULT line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

static volatile unsigned long m_regs[10];  // mscratch points here
static volatile unsigned long s_regs[10];  // sscratch points here

static volatile unsigned long pA_canary;   // phase-A lbu result
static volatile unsigned long pA_control;  // phase-A control lbu result
static volatile unsigned long pB_control;  // phase-B control lbu result
static volatile unsigned long pB_lbu_pc;   // phase-B faulting load address

static unsigned long boot_medeleg;     // medeleg at boot
static unsigned long boot_pmpcfg0;     // pmpcfg0 at boot
static unsigned long boot_pmpaddr0;    // pmpaddr0 at boot
static unsigned long boot_pmpaddr1;    // pmpaddr1 at boot
static unsigned long boot_pmpaddr2;    // pmpaddr2 at boot
static unsigned long scratch_pa;       // physical address of scratch_page
static unsigned long napot_enc;        // computed NAPOT pmpaddr value
static unsigned long pmpcfg0_rbA;      // phase-A pmpcfg0 readback
static unsigned long pmpaddr0_rbA;
static unsigned long pmpaddr1_rbA;
static unsigned long pmpaddr2_rbA;
static unsigned long pmpcfg0_rbB;      // phase-B pmpcfg0 readback
static unsigned long pmpaddr0_rbB;
static unsigned long pmpaddr1_rbB;
static unsigned long pmpaddr2_rbB;
static unsigned long rst_pmpcfg0;      // restore pmpcfg0 readback
static unsigned long rst_pmpaddr0;     // restore pmpaddr0 readback
static unsigned long rst_pmpaddr1;     // restore pmpaddr1 readback
static unsigned long rst_pmpaddr2;     // restore pmpaddr2 readback
static unsigned long pA_m_traps;       // M-mode trap count after phase A
static unsigned long pA_mcause;        // M-mode cause after phase A
static unsigned long pA_mepc;          // M-mode pc after phase A
static unsigned long pA_mstatus;       // M-mode status after phase A

static unsigned long checks = 0;
static unsigned long fails = 0;

// True when the address lies in entry 2's broad-allow window.
// noinline is load-bearing: if the comparison is inlined into the
// caller, GCC folds the window base into the PC-relative address
// materialization (auipc with a -0x80000000 addend), which
// overflows the 20-bit immediate and fails the link. With a real
// call the caller materializes the function address with a plain
// auipc and the callee compares against lui-loaded constants.
static int __attribute__((noinline)) in_broad_window(unsigned long a) {
    return a >= BROAD_BASE && a < BROAD_TOP;
}

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long csr_read_medeleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, medeleg" : "=r"(v));
    return v;
}

static unsigned long read_satp(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, satp" : "=r"(v));
    return v;
}

extern void m_trap_entry(void);
extern void s_trap_entry(void);
void smode_phaseA(void);
void smode_phaseB(void);
void phase2_mmode(void);
void report_mmode(void);

// Program pmpcfg0 and pmpaddr0/1/2, then read every register back
// into the given slots.
static void program_pmp(unsigned long cfg0, unsigned long a0,
                        unsigned long a1, unsigned long a2,
                        unsigned long *cfg_rb, unsigned long *a0_rb,
                        unsigned long *a1_rb, unsigned long *a2_rb) {
    __asm__ volatile("csrw pmpaddr0, %0\n"
                     "csrw pmpaddr1, %1\n"
                     "csrw pmpaddr2, %2\n"
                     "csrw pmpcfg0, %3"
                     :: "r"(a0), "r"(a1), "r"(a2), "r"(cfg0));
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(*cfg_rb));
    __asm__ volatile("csrr %0, pmpaddr0" : "=r"(*a0_rb));
    __asm__ volatile("csrr %0, pmpaddr1" : "=r"(*a1_rb));
    __asm__ volatile("csrr %0, pmpaddr2" : "=r"(*a2_rb));
}

// Drop from M-mode to S-mode at the given entry point. sret takes
// the target privilege from sstatus.SPP; mstatus.MPP is set to
// S-mode for a consistent view. satp is untouched (Bare). Never
// returns.
static void drop_to_smode(void (*entry)(void)) {
    unsigned long v;

    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    v |= (1UL << 8);  // SPP = 1 (S-mode)
    __asm__ volatile("csrw sstatus, %0" :: "r"(v));
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    v = (v & ~(3UL << 11)) | (1UL << 11);  // MPP = 01, consistent view
    __asm__ volatile("csrw mstatus, %0" :: "r"(v));
    __asm__ volatile("csrw sepc, %0\n"
                     "sret" :: "r"((unsigned long)entry) : "memory");
    for (;;)
        __asm__ volatile("wfi");
}

// Phase-A S-mode payload: store the resume address for the S-mode
// handler (unused; medeleg is 0, a safety net only), write the
// canary to the scratch page, read it back with lbu (entry 0's
// NAPOT R|W allow wins the priority match, so no trap), lbu the
// control byte (covered only by entry 2, completes), then ecall
// back to M-mode (cause 9, not delegated). Never returns: the
// M-mode handler redirects to phase2_mmode.
void smode_phaseA(void) {
    __asm__ volatile(
        "la t1, 2f\n"
        "sd t1, 56(%0)\n"      // s_regs[7]: resume address (safety net)
        "li t1, 0xa5\n"
        "sb t1, 0(%1)\n"       // write the canary to the scratch page
        "lbu t1, 0(%1)\n"      // read it back: entry 0 allow wins
        "sd t1, 0(%3)\n"       // pA_canary
        "lbu t1, 0(%2)\n"      // control byte: entry 2 only
        "sd t1, 0(%4)\n"       // pA_control
        "2:\n"
        "ecall\n"              // back to M-mode; cause 9, not delegated
        :: "r"(s_regs), "r"(scratch_pa), "r"(&control_byte),
           "r"(&pA_canary), "r"(&pA_control)
        : "t1", "memory");

    for (;;)  // unreachable: the M-mode handler never resumes here
        __asm__ volatile("wfi");
}

// M-mode phase 2, entered by the M-mode trap handler redirecting
// mepc here after the phase-A ecall. Snapshots the phase-A M-mode
// trap state before phase B can overwrite it, consumes the old
// continuation, rewrites pmpcfg0 with entries 0/1 swapped (entry 0
// = NAPOT deny, entry 1 = NAPOT R|W allow; entry 2 unchanged),
// verifies every readback, arms the report continuation, and drops
// to S-mode for phase B.
void phase2_mmode(void) {
    pA_m_traps = m_regs[0];
    pA_mcause = m_regs[2];
    pA_mepc = m_regs[3];
    pA_mstatus = m_regs[4];

    program_pmp(PMPCFG0_PHASEB, napot_enc, napot_enc, PMPADDR2_BROAD,
                &pmpcfg0_rbB, &pmpaddr0_rbB, &pmpaddr1_rbB, &pmpaddr2_rbB);
    uart_puts("m-mode: phase-B pmpcfg0=");
    uart_put_hex(pmpcfg0_rbB);
    uart_puts(" pmpaddr0=");
    uart_put_hex(pmpaddr0_rbB);
    uart_puts(" pmpaddr1=");
    uart_put_hex(pmpaddr1_rbB);
    uart_puts(" pmpaddr2=");
    uart_put_hex(pmpaddr2_rbB);
    uart_puts("\n");

    m_regs[7] = (unsigned long)report_mmode;
    drop_to_smode(smode_phaseB);
}

// Phase-B S-mode payload: lbu the control byte (completes: entry 2
// allows it), record the faulting lbu's own address, then lbu the
// scratch page. Entry 0 (NAPOT, no permissions) wins the priority
// match over entry 1 (NAPOT R|W allow), so the load traps in
// M-mode with mcause = 0x5; the handler records it and redirects
// to report_mmode. Never returns.
void smode_phaseB(void) {
    __asm__ volatile(
        "lbu t1, 0(%1)\n"
        "sd t1, 0(%2)\n"       // pB_control: entry 2 only, must complete
        "la t1, 0f\n"
        "sd t1, 0(%0)\n"       // pB_lbu_pc: faulting load address
        "0:\n"
        "lbu t1, 0(%3)\n"      // load from the scratch page: traps
        :: "r"(&pB_lbu_pc), "r"(&control_byte), "r"(&pB_control),
           "r"(scratch_pa)
        : "t1", "memory");

    for (;;)  // unreachable: the M-mode handler never resumes here
        __asm__ volatile("wfi");
}

// M-mode report, entered by the M-mode trap handler redirecting
// mepc here after the phase-B load access fault. Restores the boot
// PMP config and medeleg, prints every measured value, runs the
// checks, takes a quiet window, and prints the verdict. Runs in
// M-mode, which unlocked PMP entries never bind, so the UART and
// the save areas are plain physical accesses.
void report_mmode(void) {
    unsigned long h;

    // The machine is left as found. The restore readbacks go into
    // their own slots so the phase-B readbacks stay intact for the
    // checks below.
    program_pmp(boot_pmpcfg0, boot_pmpaddr0, boot_pmpaddr1, boot_pmpaddr2,
                &rst_pmpcfg0, &rst_pmpaddr0, &rst_pmpaddr1, &rst_pmpaddr2);
    __asm__ volatile("csrw medeleg, %0" :: "r"(boot_medeleg));
    h = csr_read_medeleg();

    uart_puts("pmp-priority: PMP entry-priority first-match test\n");
    uart_puts("boot: medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts(" restored readback=");
    uart_put_hex(h);
    uart_puts("\n");
    uart_puts("boot: pmpcfg0=");
    uart_put_hex(boot_pmpcfg0);
    uart_puts(" pmpaddr0=");
    uart_put_hex(boot_pmpaddr0);
    uart_puts(" pmpaddr1=");
    uart_put_hex(boot_pmpaddr1);
    uart_puts(" pmpaddr2=");
    uart_put_hex(boot_pmpaddr2);
    uart_puts("\n");
    uart_puts("restore: pmpcfg0 readback=");
    uart_put_hex(rst_pmpcfg0);
    uart_puts(" pmpaddr0=");
    uart_put_hex(rst_pmpaddr0);
    uart_puts(" pmpaddr1=");
    uart_put_hex(rst_pmpaddr1);
    uart_puts(" pmpaddr2=");
    uart_put_hex(rst_pmpaddr2);
    uart_puts("\n");
    uart_puts("scratch: pa=");
    uart_put_hex(scratch_pa);
    uart_puts(" napot=");
    uart_put_hex(napot_enc);
    uart_puts(" control=");
    uart_put_hex((unsigned long)&control_byte);
    uart_puts("\n");
    uart_puts("pmpA: cfg0=");
    uart_put_hex(pmpcfg0_rbA);
    uart_puts(" addr0=");
    uart_put_hex(pmpaddr0_rbA);
    uart_puts(" addr1=");
    uart_put_hex(pmpaddr1_rbA);
    uart_puts(" addr2=");
    uart_put_hex(pmpaddr2_rbA);
    uart_puts("\n");
    uart_puts("pmpB: cfg0=");
    uart_put_hex(pmpcfg0_rbB);
    uart_puts(" addr0=");
    uart_put_hex(pmpaddr0_rbB);
    uart_puts(" addr1=");
    uart_put_hex(pmpaddr1_rbB);
    uart_puts(" addr2=");
    uart_put_hex(pmpaddr2_rbB);
    uart_puts("\n");

    uart_puts("pA: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" canary=");
    uart_put_hex(pA_canary);
    uart_puts(" control=");
    uart_put_hex(pA_control);
    uart_puts("\n");
    uart_puts("pA: m_traps=");
    uart_put_dec(pA_m_traps);
    uart_puts(" mcause=");
    uart_put_hex(pA_mcause);
    uart_puts(" mepc=");
    uart_put_hex(pA_mepc);
    uart_puts(" mstatus_mpp=");
    uart_put_dec((pA_mstatus >> 11) & 3UL);
    uart_puts("\n");

    uart_puts("pB: m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" mcause=");
    uart_put_hex(m_regs[2]);
    uart_puts(" mepc=");
    uart_put_hex(m_regs[3]);
    uart_puts(" expected=");
    uart_put_hex(pB_lbu_pc);
    uart_puts(" mtval=");
    uart_put_hex(m_regs[8]);
    uart_puts(" mstatus_mpp=");
    uart_put_dec(((m_regs[4] >> 11) & 3UL));
    uart_puts(" control=");
    uart_put_hex(pB_control);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts("\n");

    // Boot PMP config and medeleg restored.
    check(h == boot_medeleg, "medeleg restore readback != boot medeleg");
    check(rst_pmpcfg0 == boot_pmpcfg0,
          "pmpcfg0 restore readback != boot pmpcfg0");
    check(rst_pmpaddr0 == boot_pmpaddr0,
          "pmpaddr0 restore readback != boot pmpaddr0");
    check(rst_pmpaddr1 == boot_pmpaddr1,
          "pmpaddr1 restore readback != boot pmpaddr1");
    check(rst_pmpaddr2 == boot_pmpaddr2,
          "pmpaddr2 restore readback != boot pmpaddr2");

    // Phase-A configuration: entry 0 NAPOT R|W allow and entry 1
    // NAPOT deny over the same 4 KiB page (same pmpaddr), entry 2
    // broad NAPOT R|W|X allow over RAM.
    check(pmpcfg0_rbA == PMPCFG0_PHASEA,
          "phase-A pmpcfg0 readback != 0x001f181b");
    check(pmpaddr0_rbA == napot_enc,
          "phase-A pmpaddr0 readback != computed NAPOT value");
    check(pmpaddr1_rbA == napot_enc,
          "phase-A pmpaddr1 readback != computed NAPOT value");
    check(pmpaddr2_rbA == PMPADDR2_BROAD,
          "phase-A pmpaddr2 readback != broad-allow NAPOT value");
    check((scratch_pa & (PAGE_SIZE - 1)) == 0, "scratch page misaligned");
    check(napot_enc == ((scratch_pa >> 2) | NAPOT_4K_MASK),
          "NAPOT encoding != (pa >> 2) | 0x1ff");
    check(scratch_pa >= BROAD_BASE &&
          scratch_pa + PAGE_SIZE <= BROAD_TOP,
          "scratch page outside the broad-allow window");
    check((unsigned long)&control_byte < scratch_pa ||
          (unsigned long)&control_byte >= scratch_pa + PAGE_SIZE,
          "control byte sits on the scratch page");
    check(in_broad_window((unsigned long)smode_phaseA),
          "phase-A payload outside the broad-allow window");
    check(in_broad_window((unsigned long)smode_phaseB),
          "phase-B payload outside the broad-allow window");

    // Phase-A behavior: entry 0 (allow) wins the priority match, so
    // the S-mode lbu completes with the canary and no trap fires;
    // the only M-mode trap is the phase-A return ecall.
    check(s_regs[0] == 0, "phase-A S-mode trap fired on an allowed load");
    check(pA_canary == 0xA5, "phase-A canary readback != 0xa5");
    check(pA_control == 0x5A, "phase-A control load != 0x5a");
    check(pA_m_traps == 1, "phase-A M-mode trap did not fire exactly once");
    check(pA_mcause == CAUSE_ECALL_FROM_S,
          "phase-A M-mode cause != 9 (ecall from S-mode)");
    check(((pA_mstatus >> 11) & 3UL) == 1,
          "phase-A ecall did not arrive from S-mode (mstatus.MPP)");

    // Phase-B configuration: entries 0/1 swapped, entry 2 unchanged.
    check(pmpcfg0_rbB == PMPCFG0_PHASEB,
          "phase-B pmpcfg0 readback != 0x001f1b18");
    check(pmpaddr0_rbB == napot_enc,
          "phase-B pmpaddr0 readback != computed NAPOT value");
    check(pmpaddr1_rbB == napot_enc,
          "phase-B pmpaddr1 readback != computed NAPOT value");
    check(pmpaddr2_rbB == PMPADDR2_BROAD,
          "phase-B pmpaddr2 readback != broad-allow NAPOT value");

    // Phase-B behavior: entry 0 (deny) wins the priority match, so
    // the same S-mode lbu traps in M-mode with mcause = 0x5, mepc
    // at the faulting load, mtval at the scratch page, while the
    // control load on entry 2 still completes and S-mode never
    // traps.
    check(pB_control == 0x5A, "phase-B control load != 0x5a");
    check(m_regs[0] == 2, "phase-B M-mode trap did not fire exactly once more");
    check(m_regs[2] == CAUSE_LOAD_ACCESS_FAULT,
          "phase-B mcause != 5 (load access fault)");
    check(m_regs[3] == pB_lbu_pc, "phase-B mepc != faulting load address");
    check(m_regs[8] == scratch_pa, "phase-B mtval != scratch page address");
    check(((m_regs[4] >> 11) & 3UL) == 1,
          "phase-B trap did not arrive from S-mode (mstatus.MPP)");
    check(s_regs[0] == 0, "S-mode trap fired in phase B");

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
    check(m_regs[0] == 2 && s_regs[0] == 0,
          "trap count moved during the quiet window");

    // FNV-1a over the verdict values, so the three runs can be
    // compared byte for byte.
    h = 0xcbf29ce484222325UL;
    {
        unsigned long vals[21] = {
            pmpcfg0_rbA, pmpaddr0_rbA, pmpaddr1_rbA, pmpaddr2_rbA,
            pmpcfg0_rbB, pmpaddr0_rbB, pmpaddr1_rbB, pmpaddr2_rbB,
            scratch_pa, napot_enc,
            s_regs[0], pA_canary, pA_control,
            pA_m_traps, pA_mcause, pA_mepc,
            pB_control, m_regs[0], m_regs[2], m_regs[3], m_regs[8]
        };
        unsigned long i, b;
        for (i = 0; i < 21; i++)
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
    unsigned long satp;

    uart_init();
    uart_puts("pmp-priority: PMP entry-priority first-match test\n");

    // M-mode trap handler: direct-mode mtvec, mscratch at m_regs.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // S-mode trap handler: direct-mode stvec, sscratch at s_regs.
    // Safety net only; medeleg stays 0, so no S-mode trap can be
    // delegated.
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_regs));

    // Boot-time PMP config and medeleg, for the record and the
    // end-of-run restore.
    boot_medeleg = csr_read_medeleg();
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(boot_pmpcfg0));
    __asm__ volatile("csrr %0, pmpaddr0" : "=r"(boot_pmpaddr0));
    __asm__ volatile("csrr %0, pmpaddr1" : "=r"(boot_pmpaddr1));
    __asm__ volatile("csrr %0, pmpaddr2" : "=r"(boot_pmpaddr2));
    uart_puts("m-mode: boot medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts(" pmpcfg0=");
    uart_put_hex(boot_pmpcfg0);
    uart_puts(" pmpaddr0=");
    uart_put_hex(boot_pmpaddr0);
    uart_puts(" pmpaddr1=");
    uart_put_hex(boot_pmpaddr1);
    uart_puts(" pmpaddr2=");
    uart_put_hex(boot_pmpaddr2);
    uart_puts("\n");

    // The scratch page's physical address (satp stays Bare, so the
    // virtual address is the physical address) and its NAPOT
    // pmpaddr encoding.
    scratch_pa = (unsigned long)&scratch_page[0];
    napot_enc = (scratch_pa >> 2) | NAPOT_4K_MASK;

    // Phase-A PMP layout, lowest-numbered match wins: entry 0 =
    // NAPOT R|W allow over the scratch page, entry 1 = NAPOT
    // no-perms deny over the SAME page (same pmpaddr value), entry
    // 2 = NAPOT R|W|X allow over [0x80000000, 0x100000000) so
    // S-mode code fetch and the control load keep working. Every
    // pmpcfg/pmpaddr register is read back and verified.
    program_pmp(PMPCFG0_PHASEA, napot_enc, napot_enc, PMPADDR2_BROAD,
                &pmpcfg0_rbA, &pmpaddr0_rbA, &pmpaddr1_rbA, &pmpaddr2_rbA);
    uart_puts("m-mode: phase-A pmpcfg0=");
    uart_put_hex(pmpcfg0_rbA);
    uart_puts(" pmpaddr0=");
    uart_put_hex(pmpaddr0_rbA);
    uart_puts(" pmpaddr1=");
    uart_put_hex(pmpaddr1_rbA);
    uart_puts(" pmpaddr2=");
    uart_put_hex(pmpaddr2_rbA);
    uart_puts("\n");

    // Disarm every interrupt enable: mie clear and mstatus.MIE
    // clear. sie and sstatus.SIE are never set; the only traps in
    // this run are the phase-A ecall and the phase-B access fault.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrc mstatus, %0" :: "r"(1UL << 3));  // MIE off

    // satp stays Bare: no page tables are needed, so the probe
    // address is the physical scratch-page address.
    satp = read_satp();
    uart_puts("m-mode: satp=");
    uart_put_hex(satp);
    uart_puts(" (Bare)\n");

    // Arm the phase-2 continuation the M-mode handler jumps to after
    // recording the phase-A ecall.
    m_regs[7] = (unsigned long)phase2_mmode;

    // Drop to S-mode; phase A runs in smode_phaseA.
    uart_puts("m-mode: entering S-mode (phase A, entry 0 allows)\n");
    drop_to_smode(smode_phaseA);
}
