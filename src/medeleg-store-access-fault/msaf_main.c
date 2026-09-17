// msaf_main.c: medeleg bit-7 store-access-fault trap destination
// switch (backlog item "riscv medeleg-store-access-fault").
//
// Exactly one mechanism is under test: the destination of a store
// access fault raised in S-mode as medeleg bit 7 flips. A PMP
// deny raises an access fault (mcause = 7), never a page fault,
// because PMP is checked on the physical address after the page
// walk succeeds. This module is the sibling of the done module
// src/medeleg-store-pagefault (bit 15, store/AMO page fault on an
// invalid PTE); here the leaf PTE over the store page is VALID
// with R|W, so translation succeeds and the fault comes from a
// locked PMP TOR deny entry covering exactly the store page.
//
// M-mode builds the same minimal Sv39 table by hand: root[2] ->
// l1_id[0] -> l0_id[], identity-mapping [0x80000000, 0x80080000)
// with R|W|X, plus l0_id[128] valid with R|W (no X) over the
// store page at 0x80080000. The PMP layout is three TOR entries,
// lowest-numbered match wins: entry 0 = TOR allow [0, 0x80080000)
// unlocked, entry 1 = LOCKED TOR deny [0x80080000, 0x80081000),
// entry 2 = TOR allow [0x80081000, 0x100000000) unlocked. Entry 0
// alone as TOR cannot express the page (TOR entry 0 covers from
// zero), so the allow/deny/allow triple is needed. Every pmpcfg
// and pmpaddr register is read back and verified. M-mode is never
// translated, so M-mode never touches the denied page; the
// locked entry only bites the S-mode store.
//
// Phase A runs with medeleg bit 7 set: an S-mode store to the
// denied page must trap in S-mode with scause = 7 (store/AMO
// access fault), sepc at the faulting store instruction, and
// stval holding the faulting data address, with zero M-mode traps
// from the fault itself. Phase B runs with medeleg bit 7 clear:
// the same store must trap in M-mode with mcause = 7, mepc at
// the faulting store, and mtval holding the faulting data
// address, with the S-mode trap count unchanged.
//
// Controls: each phase first stores to a non-denied scratch word
// inside the identity window; it completes with no trap, which
// proves the fault comes from the PMP entry and not from the
// store path itself. A quiet spin window moves neither trap
// counter, and the boot medeleg is restored and reported at the
// end.
//
// Sequence:
//   M-mode: install the trap handlers, record the boot medeleg,
//   program the three PMP TOR entries (readbacks verified),
//   clear mie and mstatus.MIE, build the page tables, enable Sv39,
//   write medeleg bit 7 and read back the actual configuration
//   (bit 7 verified set), arm the phase-2 continuation in
//   m_regs[7], then drop to S-mode.
//   S-mode phase A: store the resume address, record the store
//   instruction address, store a word to the allowed scratch
//   word (control, no trap), store a word to the denied page.
//   The access fault is delegated, so the S-mode handler records
//   scause/sepc/sstatus/stval and resumes the payload at the
//   ecall, which returns to M-mode (cause 9, not delegated).
//   M-mode: the handler records the ecall and redirects mepc to
//   phase2_mmode, which snapshots the phase-A counters, clears
//   medeleg bit 7 (readback verified), arms the report
//   continuation, and drops to S-mode.
//   S-mode phase B: record the store instruction address, store
//   to the allowed scratch word (control), store a word to the
//   denied page. The fault is not delegated, so the M-mode
//   handler records mcause/mepc/mstatus/mtval and redirects to
//   the M-mode report, which restores the boot medeleg, prints
//   every measured value, runs the checks, takes a quiet window,
//   and prints the verdict. All interrupt enables stay clear for
//   the whole run, so no interrupt of either kind can fire.
//
// The fault-site address is taken with `la t1, 0f` (an in-asm
// numeric local label, never C &&label, which the toolchain
// miscompiles at -O2). The handlers never advance sepc/mepc past
// the faulting store: the S-mode handler resumes at the resume
// address the payload stored, and the M-mode handler redirects to
// an armed continuation. The checks assert sepc/mepc equal the
// recorded site, so no instruction-length arithmetic exists to
// get wrong if the store were emitted compressed.

#include "../uart.h"

#define BIT_STORE_ACCESS_FAULT 7UL
#define CAUSE_STORE_ACCESS_FAULT 7UL
#define CAUSE_ECALL_FROM_S 9UL

// The PMP-denied store page: VPN2=2, VPN1=0, VPN0=128.
#define STORE_VA 0x80080000UL
#define STORE_VPN0 128
// Identity-mapped window: 128 pages, [0x80000000, 0x80080000).
#define MAP_PAGES 128
#define MAP_BASE 0x80000000UL

// PMP TOR layout. pmpaddr holds the physical address >> 2; TOR
// entry i (i >= 1) covers [pmpaddr[i-1], pmpaddr[i]), and TOR
// entry 0 covers [0, pmpaddr[0]).
#define PMPADDR0_TOP 0x20020000UL  // 0x80080000 >> 2: end of entry-0 allow
#define PMPADDR1_TOP 0x20020400UL  // 0x80081000 >> 2: end of entry-1 deny
#define PMPADDR2_TOP 0x40000000UL  // 0x100000000 >> 2: end of entry-2 allow
// pmpcfg0: entry0 = 0x0f (TOR, R|W|X, unlocked), entry1 = 0x88
// (TOR, locked, no perms), entry2 = 0x0f (TOR, R|W|X, unlocked).
#define PMPCFG0_VAL 0x000f880fUL

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
// exact. BSS clearing zeroes every entry the setup does not write.
static unsigned long root_pt[512] __attribute__((aligned(4096)));
static unsigned long l1_id[512] __attribute__((aligned(4096)));
static unsigned long l0_id[512] __attribute__((aligned(4096)));

static volatile unsigned long m_regs[10];  // mscratch points here
static volatile unsigned long s_regs[10];  // sscratch points here

// Control-store target: a scratch word inside the identity window
// that PMP entry 0 allows. Written, never read back; its only job
// is to prove a store to a non-denied page completes with no trap.
static volatile unsigned long scratch_word;

static unsigned long boot_medeleg;    // medeleg at boot
static unsigned long pA_medeleg_rb;   // phase-A medeleg readback
static unsigned long pB_medeleg_rb;   // phase-B medeleg readback
static unsigned long pmpcfg0_rb;      // pmpcfg0 readback
static unsigned long pmpaddr0_rb;     // pmpaddr0 readback
static unsigned long pmpaddr1_rb;     // pmpaddr1 readback
static unsigned long pmpaddr2_rb;     // pmpaddr2 readback
static unsigned long pA_m_traps;      // M-mode trap count after phase A
static unsigned long pA_mcause;       // M-mode cause after phase A
static unsigned long pA_mepc;         // M-mode pc after phase A
static unsigned long pA_store_pc;     // phase-A faulting store address
static unsigned long pB_store_pc;     // phase-B faulting store address

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
// handler and the faulting store's own address, store a word to
// the allowed scratch word (control: completes with no trap),
// then store a word to the PMP-denied page. medeleg bit 7 is set,
// so the store access fault lands in the S-mode handler, which
// resumes at label 2; the ecall then returns to M-mode (cause 9
// is not delegated). The fault-site address is the STORE_VA
// constant itself, so stval must equal it exactly while sepc must
// equal the store instruction's address (they differ, which is
// what distinguishes a store access fault from an instruction
// access fault).
void smode_phaseA(void) {
    __asm__ volatile(
        "la t1, 2f\n"
        "sd t1, 56(%0)\n"     // s_regs[7]: resume address
        "la t1, 0f\n"
        "sd t1, 0(%1)\n"      // pA_store_pc: faulting store address
        "li t1, 0xcafe\n"
        "sd t1, 0(%2)\n"      // control: allowed page, must not trap
        "li t0, %3\n"
        "li t1, 0xdeadbeef\n"
        "0:\n"
        "sd t1, 0(t0)\n"      // store to the PMP-denied page: traps
        "2:\n"
        "ecall\n"             // back to M-mode; cause 9, not delegated
        :: "r"(s_regs), "r"(&pA_store_pc), "r"(&scratch_word), "i"(STORE_VA)
        : "t0", "t1", "memory");

    for (;;)  // unreachable: the ecall never returns here
        __asm__ volatile("wfi");
}

// M-mode phase 2, entered by the M-mode trap handler redirecting
// mepc here after the phase-A ecall. Snapshots the phase-A M-mode
// trap state before phase B can overwrite it, consumes the old
// continuation, clears medeleg bit 7 (readback verified), arms the
// report continuation, and drops to S-mode for phase B.
void phase2_mmode(void) {
    pA_m_traps = m_regs[0];
    pA_mcause = m_regs[2];
    pA_mepc = m_regs[3];

    m_regs[7] = (unsigned long)report_mmode;
    csr_write_medeleg(csr_read_medeleg() & ~(1UL << BIT_STORE_ACCESS_FAULT));
    pB_medeleg_rb = csr_read_medeleg();

    drop_to_smode(smode_phaseB);
}

// Phase-B S-mode payload: record the faulting store's own
// address, store a word to the allowed scratch word (control),
// then store a word to the PMP-denied page. medeleg bit 7 is
// clear, so the store access fault lands in the M-mode handler,
// which records it and redirects to report_mmode. Never returns.
void smode_phaseB(void) {
    __asm__ volatile(
        "la t1, 0f\n"
        "sd t1, 0(%0)\n"      // pB_store_pc: faulting store address
        "li t1, 0xcafe\n"
        "sd t1, 0(%1)\n"      // control: allowed page, must not trap
        "li t0, %2\n"
        "li t1, 0xdeadbeef\n"
        "0:\n"
        "sd t1, 0(t0)\n"      // store to the PMP-denied page: traps
        :: "r"(&pB_store_pc), "r"(&scratch_word), "i"(STORE_VA)
        : "t0", "t1", "memory");

    for (;;)  // unreachable: the M-mode handler never resumes here
        __asm__ volatile("wfi");
}

// M-mode report, entered by the M-mode trap handler redirecting
// mepc here after the phase-B store access fault. Restores the
// boot medeleg, prints every measured value, runs the checks,
// takes a quiet window, and prints the verdict. Runs in M-mode,
// which is never translated, so the UART and the save areas are
// plain physical accesses.
void report_mmode(void) {
    unsigned long h, rb;

    csr_write_medeleg(boot_medeleg);
    rb = csr_read_medeleg();

    uart_puts("medeleg-store-access-fault: store-access-fault trap destination switch test\n");
    uart_puts("boot: medeleg=");
    uart_put_hex(boot_medeleg);
    uart_puts(" restored readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    uart_puts("table: root[2]->l1[0]->l0 identity [0x80000000,0x80080000) R|W|X, l0[128] valid R|W (PMP-denied store page 0x80080000)\n");
    uart_puts("pmp: cfg0=");
    uart_put_hex(pmpcfg0_rb);
    uart_puts(" addr0=");
    uart_put_hex(pmpaddr0_rb);
    uart_puts(" addr1=");
    uart_put_hex(pmpaddr1_rb);
    uart_puts(" addr2=");
    uart_put_hex(pmpaddr2_rb);
    uart_puts("\n");
    uart_puts("phaseA: medeleg bit7 set readback=");
    uart_put_hex(pA_medeleg_rb);
    uart_puts("\n");
    uart_puts("phaseB: medeleg bit7 clear readback=");
    uart_put_hex(pB_medeleg_rb);
    uart_puts("\n");

    uart_puts("pA: s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts(" scause=");
    uart_put_hex(s_regs[2]);
    uart_puts(" sepc=");
    uart_put_hex(s_regs[3]);
    uart_puts(" expected=");
    uart_put_hex(pA_store_pc);
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
    uart_put_hex(pB_store_pc);
    uart_puts(" mtval=");
    uart_put_hex(m_regs[8]);
    uart_puts(" mstatus_mpp=");
    uart_put_dec((m_regs[4] >> 11) & 3UL);
    uart_puts(" s_traps=");
    uart_put_dec(s_regs[0]);
    uart_puts("\n");

    // The machine is left as found.
    check(rb == boot_medeleg, "medeleg restore readback != boot medeleg");

    // Phase-A checks: medeleg bit 7 set, the store to the
    // PMP-denied page trapped in S-mode as an access fault with
    // the faulting data address in stval and the store
    // instruction's address in sepc (they differ, which is what
    // distinguishes a store access fault from an instruction
    // access fault), the control store to the allowed page
    // completed with no trap (s_traps exactly 1), and the M-mode
    // handler saw only the ecall.
    check((pA_medeleg_rb & (1UL << BIT_STORE_ACCESS_FAULT)) != 0,
          "phase-A medeleg readback missing bit 7");
    check(s_regs[0] == 1, "phase-A S-mode trap did not fire exactly once");
    check(s_regs[2] == CAUSE_STORE_ACCESS_FAULT,
          "phase-A scause != 7 (store access fault)");
    check(s_regs[3] == pA_store_pc,
          "phase-A sepc != faulting store address");
    check(s_regs[8] == STORE_VA, "phase-A stval != store page address");
    check(s_regs[3] != s_regs[8],
          "phase-A sepc equals stval; a store fault must separate them");
    check(((s_regs[4] >> 8) & 1UL) == 1,
          "phase-A trap did not arrive from S-mode (sstatus.SPP)");
    check(pA_m_traps == 1 && pA_mcause == CAUSE_ECALL_FROM_S,
          "phase-A M-mode saw something other than the single ecall");

    // Phase-B checks: medeleg bit 7 clear, the store to the
    // PMP-denied page trapped in M-mode as an access fault with
    // the faulting data address in mtval and the store
    // instruction's address in mepc, and no S-mode trap fired
    // (the control store in phase B also completed with no trap).
    check((pB_medeleg_rb & (1UL << BIT_STORE_ACCESS_FAULT)) == 0,
          "phase-B medeleg readback still has bit 7 set");
    check(m_regs[0] == 2, "phase-B M-mode trap did not fire exactly once more");
    check(m_regs[2] == CAUSE_STORE_ACCESS_FAULT,
          "phase-B mcause != 7 (store access fault)");
    check(m_regs[3] == pB_store_pc,
          "phase-B mepc != faulting store address");
    check(m_regs[8] == STORE_VA, "phase-B mtval != store page address");
    check(((m_regs[4] >> 11) & 3UL) == 1,
          "phase-B trap did not arrive from S-mode (mstatus.MPP)");
    check(s_regs[0] == 1,
          "S-mode trap fired in phase B with bit 7 clear");

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
        unsigned long vals[14] = {
            s_regs[0], s_regs[2], s_regs[3], s_regs[8],
            m_regs[0], m_regs[2], m_regs[3], m_regs[8],
            pA_medeleg_rb, pB_medeleg_rb,
            pmpcfg0_rb, pmpaddr0_rb, pmpaddr1_rb, pmpaddr2_rb
        };
        unsigned long i, b;
        for (i = 0; i < 14; i++)
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
    unsigned long root_ppn, l1_ppn, l0_ppn, base_ppn, satp;
    int i;

    uart_init();
    uart_puts("medeleg-store-access-fault: medeleg bit-7 store-access-fault trap destination switch\n");

    // M-mode trap handler: direct-mode mtvec, mscratch at m_regs.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // S-mode trap handler: direct-mode stvec, sscratch at s_regs.
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_regs));

    // Boot-time medeleg, for the record and the end-of-run restore.
    boot_medeleg = csr_read_medeleg();

    // Three PMP TOR entries, lowest-numbered match wins: entry 0
    // allows [0, 0x80080000) R|W|X unlocked, entry 1 LOCKED denies
    // [0x80080000, 0x80081000) (exactly the store page), entry 2
    // allows [0x80081000, 0x100000000) R|W|X unlocked. The locked
    // entry also binds M-mode, which never touches the page.
    __asm__ volatile("li t0, %0\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, %1\n"
                     "csrw pmpaddr1, t0\n"
                     "li t0, %2\n"
                     "csrw pmpaddr2, t0\n"
                     "li t0, %3\n"
                     "csrw pmpcfg0, t0"
                     :: "i"(PMPADDR0_TOP), "i"(PMPADDR1_TOP),
                        "i"(PMPADDR2_TOP), "i"(PMPCFG0_VAL)
                     : "t0");
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(pmpcfg0_rb));
    __asm__ volatile("csrr %0, pmpaddr0" : "=r"(pmpaddr0_rb));
    __asm__ volatile("csrr %0, pmpaddr1" : "=r"(pmpaddr1_rb));
    __asm__ volatile("csrr %0, pmpaddr2" : "=r"(pmpaddr2_rb));
    uart_puts("m-mode: pmpcfg0=");
    uart_put_hex(pmpcfg0_rb);
    uart_puts(" pmpaddr0=");
    uart_put_hex(pmpaddr0_rb);
    uart_puts(" pmpaddr1=");
    uart_put_hex(pmpaddr1_rb);
    uart_puts(" pmpaddr2=");
    uart_put_hex(pmpaddr2_rb);
    uart_puts("\n");
    check(pmpcfg0_rb == PMPCFG0_VAL, "pmpcfg0 readback != 0x000f880f");
    check(pmpaddr0_rb == PMPADDR0_TOP, "pmpaddr0 readback != 0x20020000");
    check(pmpaddr1_rb == PMPADDR1_TOP, "pmpaddr1 readback != 0x20020400");
    check(pmpaddr2_rb == PMPADDR2_TOP, "pmpaddr2 readback != 0x40000000");

    // Disarm every interrupt enable: mie clear and mstatus.MIE
    // clear. sie and sstatus.SIE are never set; the only traps in
    // this run are the two access faults and the phase-A ecall.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrc mstatus, %0" :: "r"(1UL << 3));  // MIE off

    // Build the tables by hand. root[2] is a V-only pointer to the
    // level-1 table, l1_id[0] a V-only pointer to the level-0 table,
    // and l0_id[i] identity-maps pages [0x80000000, 0x80080000)
    // with R|W|X. l0_id[128] is VALID with R|W (no X): the store
    // page translates, so the fault must come from the PMP deny,
    // not from the page walk. Every other entry in all three
    // tables is zero.
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
    // l0_id[128]: valid R|W, the PMP-denied store page.
    l0_id[STORE_VPN0] = ((base_ppn + STORE_VPN0) << 10) |
                        (PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);

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
    check((l0_id[STORE_VPN0] & (PTE_V | PTE_R | PTE_W)) ==
          (PTE_V | PTE_R | PTE_W),
          "store page leaf not valid R|W");
    check(((l0_id[STORE_VPN0] >> 10) & 0xFFFFFFFFFFFUL) ==
          base_ppn + STORE_VPN0,
          "store page leaf PPN wrong");
    check((l0_id[STORE_VPN0] & PTE_X) == 0,
          "store page leaf has X; it is a data page");
    check((unsigned long)smode_phaseA < MAP_BASE + (MAP_PAGES << 12),
          "smode_phaseA outside identity window");
    check((unsigned long)smode_phaseB < MAP_BASE + (MAP_PAGES << 12),
          "smode_phaseB outside identity window");
    check((unsigned long)s_trap_entry < MAP_BASE + (MAP_PAGES << 12),
          "s_trap_entry outside identity window");
    check((unsigned long)&s_regs[9] < MAP_BASE + (MAP_PAGES << 12),
          "s_regs outside identity window");
    check((unsigned long)&scratch_word < MAP_BASE + (MAP_PAGES << 12),
          "scratch_word outside identity window");

    // Enable Sv39, then confirm the mode and root stuck.
    set_satp(SATP_MODE_SV39 | root_ppn);
    satp = read_satp();
    uart_puts("m-mode: satp=");
    uart_put_hex(satp);
    uart_puts(" (MODE=8 ASID=0)\n");
    check((satp >> 60) == 8, "satp MODE != 8 (Sv39)");
    check((satp & 0xFFFFFFFFFFFUL) == root_ppn, "satp PPN != root page");

    // Phase A: delegate store access faults to S-mode and read back
    // the actual configuration the first fault runs under.
    csr_write_medeleg(1UL << BIT_STORE_ACCESS_FAULT);
    pA_medeleg_rb = csr_read_medeleg();
    uart_puts("m-mode: phase-A medeleg write=0x80 readback=");
    uart_put_hex(pA_medeleg_rb);
    uart_puts("\n");

    // Arm the phase-2 continuation the M-mode handler jumps to after
    // recording the phase-A ecall.
    m_regs[7] = (unsigned long)phase2_mmode;

    // Drop to S-mode; phase A runs in smode_phaseA.
    uart_puts("m-mode: entering S-mode (phase A, bit 7 set)\n");
    drop_to_smode(smode_phaseA);
}
