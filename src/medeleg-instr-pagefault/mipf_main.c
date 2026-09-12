// mipf_main.c: medeleg bit-12 instruction-page-fault trap destination
// switch (backlog item "riscv medeleg-instr-pagefault").
//
// Exactly one mechanism is under test: the destination of an
// instruction page fault raised in S-mode as medeleg bit 12 flips.
// Phase A runs with medeleg bit 12 set: an S-mode jump to an
// unmapped page must trap in S-mode with scause = 12 (instruction
// page fault), sepc at the faulting fetch address, and stval holding
// the faulting address, with zero M-mode traps from the fault
// itself. Phase B runs with medeleg bit 12 clear: the same jump
// must trap in M-mode with mcause = 12 and mepc at the faulting
// address, with the S-mode trap count unchanged.
//
// The fault page is a deliberately unmapped 4 KiB page at virtual
// address 0x80080000. M-mode builds a minimal Sv39 table by hand:
// root[2] -> l1_id[0] -> l0_id[], with l0_id[i] identity-mapping
// pages [0x80000000, 0x80080000) with R|W|X and l0_id[128] left zero
// (invalid), so the walk dies at the leaf lookup for a fetch from
// 0x80080000. One PMP NAPOT entry grants S-mode R|W|X over the whole
// address space (unlocked entries are never checked for M-mode, so
// the M-mode phase is unaffected), and Sv39 is enabled via satp +
// sfence.vma before the drop to S-mode. M-mode is never translated,
// so the M-mode phase and both trap handlers run on physical
// addresses; the S-mode phase, the handlers' save areas, and the
// fault page arithmetic all sit inside the identity-mapped window.
//
// Sequence:
//   M-mode: install the trap handlers, record the boot medeleg,
//   open the address space with PMP, clear mie and mstatus.MIE,
//   build the page tables, enable Sv39, write medeleg bit 12 and
//   read back the actual configuration (bit 12 verified set), arm
//   the phase-2 continuation in m_regs[7], then drop to S-mode.
//   S-mode phase A: store the resume address, jump to the fault
//   page. The instruction page fault is delegated, so the S-mode
//   handler records scause/sepc/sstatus/stval and resumes the
//   payload, which issues an ecall (cause 9, not delegated) to get
//   back to M-mode.
//   M-mode: the handler records the ecall and redirects mepc to
//   phase2_mmode, which snapshots the phase-A counters, clears
//   medeleg bit 12 (readback verified), arms the report
//   continuation, and drops to S-mode.
//   S-mode phase B: jump to the fault page. The fault is not
//   delegated, so the M-mode handler records mcause/mepc/mstatus/
//   mtval and redirects to the M-mode report, which prints every
//   measured value, runs the checks, takes a quiet window, and
//   prints the verdict. All interrupt enables stay clear for the
//   whole run, so no interrupt of either kind can fire.

#include "../uart.h"

#define BIT_INSTR_PAGE_FAULT 12UL
#define CAUSE_INSTR_PAGE_FAULT 12UL
#define CAUSE_ECALL_FROM_S 9UL

// The deliberately unmapped page: VPN2=2, VPN1=0, VPN0=128.
#define FAULT_VA 0x80080000UL
#define FAULT_VPN0 128
// Identity-mapped window: 128 pages, [0x80000000, 0x80080000).
#define MAP_PAGES 128
#define MAP_BASE 0x80000000UL

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

#define MSTATUS_MPP_MASK (3UL << 11)
#define MSTATUS_MPP_S (1UL << 11)
#define SATP_MODE_SV39 (8UL << 60)

// Page-table pages, each a full 4 KiB page so the PPN arithmetic is
// exact. BSS clearing zeroes every other entry (invalid), which is
// what the fault page relies on: l0_id[128] stays zero.
static unsigned long root_pt[512] __attribute__((aligned(4096)));
static unsigned long l1_id[512] __attribute__((aligned(4096)));
static unsigned long l0_id[512] __attribute__((aligned(4096)));

static volatile unsigned long m_regs[10];  // mscratch points here
static volatile unsigned long s_regs[10];  // sscratch points here

static unsigned long boot_medeleg;    // medeleg at boot
static unsigned long pA_medeleg_rb;   // phase-A medeleg readback
static unsigned long pB_medeleg_rb;   // phase-B medeleg readback
static unsigned long pA_m_traps;      // M-mode trap count after phase A
static unsigned long pA_mcause;       // M-mode cause after phase A
static unsigned long pA_mepc;         // M-mode pc after phase A

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

static void csr_write_medeleg(unsigned long v) {
    __asm__ volatile("csrw medeleg, %0" :: "r"(v));
}

static unsigned long csr_read_medeleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, medeleg" : "=r"(v));
    return v;
}

static void set_satp(unsigned long v) {
    __asm__ volatile("csrw satp, %0" :: "r"(v));
    __asm__ volatile("sfence.vma" ::: "memory");
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

// Drop from M-mode to S-mode at the given entry point. sret takes
// the target privilege from sstatus.SPP; mstatus.MPP is set to
// S-mode for a consistent view. Never returns.
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
// handler, then jump to the unmapped fault page. medeleg bit 12 is
// set, so the instruction page fault lands in the S-mode handler,
// which resumes at label 1; the ecall then returns to M-mode
// (cause 9 is not delegated). The fault-site address is the
// FAULT_VA constant itself, so sepc/stval must equal it exactly.
void smode_phaseA(void) {
    __asm__ volatile(
        "la t0, 1f\n"
        "sd t0, 56(%0)\n"     // s_regs[7]: resume address
        "li t0, %1\n"
        "jalr zero, 0(t0)\n"  // fetch from the unmapped page: traps
        "1:\n"
        "ecall\n"             // back to M-mode; cause 9, not delegated
        :: "r"(s_regs), "i"(FAULT_VA)
        : "t0", "memory");

    for (;;)  // unreachable: the ecall never returns here
        __asm__ volatile("wfi");
}

// M-mode phase 2, entered by the M-mode trap handler redirecting
// mepc here after the phase-A ecall. Snapshots the phase-A M-mode
// trap state before phase B can overwrite it, consumes the old
// continuation, clears medeleg bit 12 (readback verified), arms the
// report continuation, and drops to S-mode for phase B.
void phase2_mmode(void) {
    pA_m_traps = m_regs[0];
    pA_mcause = m_regs[2];
    pA_mepc = m_regs[3];

    m_regs[7] = (unsigned long)report_mmode;
    csr_write_medeleg(csr_read_medeleg() & ~(1UL << BIT_INSTR_PAGE_FAULT));
    pB_medeleg_rb = csr_read_medeleg();

    drop_to_smode(smode_phaseB);
}

// Phase-B S-mode payload: jump to the fault page. medeleg bit 12 is
// clear, so the instruction page fault lands in the M-mode handler,
// which records it and redirects to report_mmode. Never returns.
void smode_phaseB(void) {
    __asm__ volatile(
        "li t0, %0\n"
        "jalr zero, 0(t0)\n"  // fetch from the unmapped page: traps
        :: "i"(FAULT_VA)
        : "t0", "memory");

    for (;;)  // unreachable: the M-mode handler never resumes here
        __asm__ volatile("wfi");
}

// M-mode report, entered by the M-mode trap handler redirecting
// mepc here after the phase-B instruction page fault. Prints every
// measured value, runs the checks, takes a quiet window, and prints
// the verdict. Runs in M-mode, which is never translated, so the
// UART and the save areas are plain physical accesses.
void report_mmode(void) {
    unsigned long h;

    uart_puts("medeleg-instr-pagefault: instruction-page-fault trap destination switch test\n");
    uart_puts("boot: medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts("\n");
    uart_puts("table: root[2]->l1[0]->l0 identity [0x80000000,0x80080000) R|W|X, l0[128]=0 (fault page 0x80080000)\n");
    uart_puts("phaseA: medeleg bit12 set readback=");
    uart_put_hex(pA_medeleg_rb);
    uart_puts("\n");
    uart_puts("phaseB: medeleg bit12 clear readback=");
    uart_put_hex(pB_medeleg_rb);
    uart_puts("\n");

    uart_puts("pA: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" scause=");
    uart_put_hex(s_regs[2]);
    uart_puts(" sepc=");
    uart_put_hex(s_regs[3]);
    uart_puts(" expected=");
    uart_put_hex(FAULT_VA);
    uart_puts(" stval=");
    uart_put_hex(s_regs[8]);
    uart_puts(" sstatus_spp=");
    uart_put_dec((s_regs[4] >> 8) & 1UL);
    uart_puts("\n");
    uart_puts("pA: m_traps=");
    uart_put_dec(pA_m_traps);
    uart_puts(" mcause=");
    uart_put_hex(pA_mcause);
    uart_puts(" mepc=");
    uart_put_hex(pA_mepc);
    uart_puts("\n");

    uart_puts("pB: m_traps=");
    uart_put_dec(m_regs[0]);
    uart_puts(" mcause=");
    uart_put_hex(m_regs[2]);
    uart_puts(" mepc=");
    uart_put_hex(m_regs[3]);
    uart_puts(" expected=");
    uart_put_hex(FAULT_VA);
    uart_puts(" mtval=");
    uart_put_hex(m_regs[8]);
    uart_puts(" mstatus_mpp=");
    uart_put_dec((m_regs[4] >> 11) & 3UL);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts("\n");

    // Phase-A checks: medeleg bit 12 set, the instruction page
    // fault trapped in S-mode with the faulting address in both
    // sepc and stval, and the M-mode handler saw only the ecall.
    check((pA_medeleg_rb & (1UL << BIT_INSTR_PAGE_FAULT)) != 0,
          "phase-A medeleg readback missing bit 12");
    check(s_regs[0] == 1, "phase-A S-mode trap did not fire exactly once");
    check(s_regs[2] == CAUSE_INSTR_PAGE_FAULT,
          "phase-A scause != 12 (instruction page fault)");
    check(s_regs[3] == FAULT_VA, "phase-A sepc != fault page address");
    check(s_regs[8] == FAULT_VA, "phase-A stval != fault page address");
    check(((s_regs[4] >> 8) & 1UL) == 1,
          "phase-A trap did not arrive from S-mode (sstatus.SPP)");
    check(pA_m_traps == 1 && pA_mcause == CAUSE_ECALL_FROM_S,
          "phase-A M-mode saw something other than the single ecall");

    // Phase-B checks: medeleg bit 12 clear, the instruction page
    // fault trapped in M-mode with the faulting address in both
    // mepc and mtval, and no S-mode trap fired.
    check((pB_medeleg_rb & (1UL << BIT_INSTR_PAGE_FAULT)) == 0,
          "phase-B medeleg readback still has bit 12 set");
    check(m_regs[0] == 2, "phase-B M-mode trap did not fire exactly once more");
    check(m_regs[2] == CAUSE_INSTR_PAGE_FAULT,
          "phase-B mcause != 12 (instruction page fault)");
    check(m_regs[3] == FAULT_VA, "phase-B mepc != fault page address");
    check(m_regs[8] == FAULT_VA, "phase-B mtval != fault page address");
    check(((m_regs[4] >> 11) & 3UL) == 1,
          "phase-B trap did not arrive from S-mode (mstatus.MPP)");
    check(s_regs[0] == 1,
          "S-mode trap fired in phase B with bit 12 clear");

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
    check(m_regs[0] == 2 && s_regs[0] == 1,
          "trap count moved during the quiet window");

    // FNV-1a over the verdict values, so the three runs can be
    // compared byte for byte.
    h = 0xcbf29ce484222325UL;
    {
        unsigned long vals[10] = {
            s_regs[0], s_regs[2], s_regs[3], s_regs[8],
            m_regs[0], m_regs[2], m_regs[3], m_regs[8],
            pA_medeleg_rb, pB_medeleg_rb
        };
        unsigned long i, b;
        for (i = 0; i < 10; i++)
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
    unsigned long root_ppn, l1_ppn, l0_ppn, base_ppn, satp, rb;
    int i;

    uart_init();
    uart_puts("medeleg-instr-pagefault: medeleg bit-12 instruction-page-fault trap destination switch\n");

    // M-mode trap handler: direct-mode mtvec, mscratch at m_regs.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // S-mode trap handler: direct-mode stvec, sscratch at s_regs.
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_regs));

    // Boot-time medeleg, for the record.
    boot_medeleg = csr_read_medeleg();

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
    // clear. sie and sstatus.SIE are never set; the only traps in
    // this run are the two page faults and the phase-A ecall.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrc mstatus, %0" :: "r"(1UL << 3));  // MIE off

    // Build the tables by hand. root[2] is a V-only pointer to the
    // level-1 table, l1_id[0] a V-only pointer to the level-0 table,
    // and l0_id[i] identity-maps pages [0x80000000, 0x80080000)
    // with R|W|X. l0_id[128] stays zero (invalid): the fault page.
    // Every other entry in all three tables is zero.
    check(((unsigned long)root_pt & 0xFFFUL) == 0, "root_pt misaligned");
    check(((unsigned long)l1_id & 0xFFFUL) == 0, "l1_id misaligned");
    check(((unsigned long)l0_id & 0xFFFUL) == 0, "l0_id misaligned");
    root_ppn = (unsigned long)root_pt >> 12;
    l1_ppn = (unsigned long)l1_id >> 12;
    l0_ppn = (unsigned long)l0_id >> 12;
    base_ppn = MAP_BASE >> 12;
    root_pt[2] = (l1_ppn << 10) | PTE_V;
    l1_id[0] = (l0_ppn << 10) | PTE_V;
    for (i = 0; i < MAP_PAGES; i++)
        l0_id[i] = ((base_ppn + (unsigned long)i) << 10) |
                   (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);
    // l0_id[128]: deliberately left invalid (the fault page).

    check((root_pt[2] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "root entry 2 is not a pure pointer (V only)");
    check(((root_pt[2] >> 10) & 0xFFFFFFFFFFFUL) == l1_ppn,
          "root[2] PPN != l1_id page number");
    check((l1_id[0] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "l1 entry 0 is not a pure pointer (V only)");
    check(((l1_id[0] >> 10) & 0xFFFFFFFFFFFUL) == l0_ppn,
          "l1_id[0] PPN != l0_id page number");
    check((l0_id[0] & (PTE_V | PTE_R | PTE_W | PTE_X)) ==
          (PTE_V | PTE_R | PTE_W | PTE_X),
          "leaf 0 missing V/R/W/X");
    check(((l0_id[0] >> 10) & 0xFFFFFFFFFFFUL) == base_ppn,
          "leaf 0 PPN != 0x80000");
    check((l0_id[MAP_PAGES - 1] & PTE_V) != 0,
          "leaf 127 unexpectedly invalid");
    check(l0_id[FAULT_VPN0] == 0, "fault page PTE is not invalid");
    check((unsigned long)report_mmode < MAP_BASE + (MAP_PAGES << 12),
          "report_mmode outside identity window");
    check((unsigned long)s_trap_entry < MAP_BASE + (MAP_PAGES << 12),
          "s_trap_entry outside identity window");
    check((unsigned long)&s_regs[9] < MAP_BASE + (MAP_PAGES << 12),
          "s_regs outside identity window");

    // Enable Sv39, then confirm the mode and root stuck.
    set_satp(SATP_MODE_SV39 | root_ppn);
    satp = read_satp();
    uart_puts("m-mode: satp=");
    uart_put_hex(satp);
    uart_puts(" (MODE=8 ASID=0)\n");
    check((satp >> 60) == 8, "satp MODE != 8 (Sv39)");
    check((satp & 0xFFFFFFFFFFFUL) == root_ppn, "satp PPN != root page");

    // Phase A: delegate instruction page faults to S-mode and read
    // back the actual configuration the first fault runs under.
    csr_write_medeleg(1UL << BIT_INSTR_PAGE_FAULT);
    pA_medeleg_rb = csr_read_medeleg();
    uart_puts("m-mode: phase-A medeleg write=0x1000 readback=");
    uart_put_hex(pA_medeleg_rb);
    uart_puts("\n");

    // Arm the phase-2 continuation the M-mode handler jumps to after
    // recording the phase-A ecall.
    m_regs[7] = (unsigned long)phase2_mmode;

    // Drop to S-mode; phase A runs in smode_phaseA.
    uart_puts("m-mode: entering S-mode (phase A, bit 12 set)\n");
    drop_to_smode(smode_phaseA);
}
