// sv39_main.c: Sv39 two-level page-table walk and fault path (backlog item 39).
//
// A bare-metal program that builds a minimal Sv39 page table by hand in
// RAM, enables it via satp, and exercises the hardware walk from
// S-mode:
//
//   1. A store and a load through a mapped virtual address, proving the
//      walk resolves VA -> PA.
//   2. A load from a VA whose level-1 PTE is invalid (the walk dies at
//      the second lookup), proving the fault path reports load page
//      fault with mtval = the faulting VA.
//   3. A load from a VA whose level-0 PTE is invalid (the walk dies at
//      the third lookup).
//   4. A load from a VA whose root PTE is invalid (the walk dies at the
//      first lookup).
//
// The mapped page is a real 4 KiB page, so the test mapping is a full
// three-level walk: root[1] -> l1_test[0] -> l0_test[0] -> data page.
// (A 4 KiB leaf is only valid in a level-0 table; a leaf one level up
// would be a 2 MiB superpage, and QEMU rejects the walk with a
// misaligned-PPN page fault, which is how this was verified.)
//
// The program boots in M-mode (QEMU loads the ELF straight into M-mode
// with -bios none). M-mode builds the tables, writes satp (MODE=8,
// ASID 0), issues sfence.vma, installs the M-mode trap vector, then
// drops to S-mode with mret. S-mode does the translated accesses; all
// traps go to the M-mode handler, which records mcause/mepc/mtval and
// resumes the test.
//
// Why S-mode and not M-mode with MPRV: on QEMU 8.2.2, explicit M-mode
// accesses with mstatus.MPRV=1/MPP=S are not translated. The TCG
// frontend computes each translated block's mmu_idx once at block
// translation time, and QEMU 8.2.2's write_mstatus only flushes the TLB
// when the MXR bit changes, so a csrw mstatus inside a block does not
// switch the block to the S-mode mmu_idx; the access runs as a physical
// M-mode access (observed: load access fault, mcause=5, at the physical
// address). Running the test phase in S-mode is the canonical way to
// exercise translation and sidesteps the emulator quirk entirely.
//
// The table therefore also carries the small identity scaffolding the
// S-mode phase needs: a 1 GiB megapage at root[2] identity-mapping
// [0x80000000, 0xC0000000) (code, data, stack, the tables themselves;
// a level-2 leaf covers 2^18 pages = 1 GiB), and a root[0] -> l1_uart ->
// l0_uart chain mapping one 4 KiB UART page at 0x10000000 so S-mode can
// print. The mechanism under test is unchanged: one 4 KiB page mapped
// at VA 0x40000000 through root[1] -> l1_test[0] -> l0_test[0].
//
// One more piece of scaffolding the spec demands: with PMP implemented,
// an S-mode access that matches no PMP entry fails, so M-mode programs
// one TOR PMP entry granting the S-mode phase R|W|X over [0, 0x80400000)
// before dropping privilege. Unlocked PMP entries are not checked for
// M-mode, so the M-mode phase is unaffected.

#include "../uart.h"

extern void sv39_trap_entry(void);
extern char _stack_top;

// Trap save area, laid out for sv39_trap.S:
// [1]=t1 [2]=mcause [3]=mepc [4]=mtval [5]=resume pc [6]=seen flag.
// mscratch points here while a test is armed.
volatile unsigned long sv39_save[8];

// Page-table pages and the one mapped data page, each a full 4 KiB page
// so the PPN arithmetic is exact. BSS clearing zeroes every other
// entry (invalid), which is what the fault tests rely on.
static unsigned long root_pt[512] __attribute__((aligned(4096)));
static unsigned long l1_test[512] __attribute__((aligned(4096)));
static unsigned long l0_test[512] __attribute__((aligned(4096)));
static unsigned long l1_uart[512] __attribute__((aligned(4096)));
static unsigned long l0_uart[512] __attribute__((aligned(4096)));
static volatile unsigned char page[4096] __attribute__((aligned(4096)));

// The one mapped virtual address: VPN[2]=1, VPN[1]=0, VPN[0]=0.
#define MAPPED_VA 0x40000000UL
// Walk dies at level 0: root and l1 entries valid, l0_test[1] is zero
// (VPN[0] of 0x40001000 is 1).
#define FAULT_VA_L0 0x40001000UL
// Walk dies at level 1: root entry valid, l1_test[128] is zero
// (VPN[1] of 0x50000000 is 128).
#define FAULT_VA_L1 0x50000000UL
// Walk dies at level 2: root_pt[3] is zero (VPN[2] of 0xC0000000 is 3).
#define FAULT_VA_L2 0xC0000000UL

// Identity-mapped window for the S-mode phase (1 GiB megapage).
#define IDENT_BASE 0x80000000UL
#define IDENT_END  0xC0000000UL
#define UART_BASE  0x10000000UL

#define PATTERN 0xDEADBEEFCAFEBABEUL

// PTE flag bits (RISC-V privileged spec, Sv39 PTE format).
#define PTE_V 0x001UL
#define PTE_R 0x002UL
#define PTE_W 0x004UL
#define PTE_X 0x008UL
#define PTE_U 0x010UL
#define PTE_G 0x020UL
#define PTE_A 0x040UL
#define PTE_D 0x080UL

#define MSTATUS_MPP_MASK (3UL << 11)
#define MSTATUS_MPP_S (1UL << 11)
#define SATP_MODE_SV39 (8UL << 60)

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long csr_read_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
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

// Store the pattern through the mapping and load it back. Runs in
// S-mode with translation on. The M-mode trap handler resumes at label
// 1 if the walk faults, so seen=1 afterwards means the mapping did not
// resolve.
static unsigned long test_mapped(void) {
    unsigned long got = 0;

    __asm__ volatile(
        "la t0, 1f\n"
        "sd t0, 40(%1)\n"      // sv39_save[5]: resume pc
        "sd zero, 48(%1)\n"    // sv39_save[6]: seen = 0
        "sd %3, 0(%2)\n"       // store pattern via VA 0x40000000
        "ld %0, 0(%2)\n"       // load it back via the same VA
        "1:\n"
        : "=r"(got)
        : "r"(sv39_save), "r"(MAPPED_VA), "r"(PATTERN)
        : "t0", "memory");
    return got;
}

// Attempt a load from an unmapped VA; the M-mode trap handler records
// mcause/mepc/mtval and resumes at label 1. The faulting ld is 4 bytes
// at (auipc address)+4 (t1 is not a compressible register), so mepc
// must equal resume_pc - 4.
static void test_fault(unsigned long va, const char *name) {
    __asm__ volatile(
        "la t0, 1f\n"
        "sd t0, 40(%0)\n"      // sv39_save[5]: resume pc
        "sd zero, 48(%0)\n"    // sv39_save[6]: seen = 0
        "auipc t0, 0\n"        // A: address of this auipc
        "ld t1, 0(%1)\n"       // faulting load, at A+4
        "1:\n"
        :
        : "r"(sv39_save), "r"(va)
        : "t0", "t1", "memory");

    uart_puts(name);
    uart_puts(": seen=");
    uart_put_dec(sv39_save[6]);
    uart_puts(" mcause=");
    uart_put_hex(sv39_save[2]);
    uart_puts(" mepc=");
    uart_put_hex(sv39_save[3]);
    uart_puts(" mtval=");
    uart_put_hex(sv39_save[4]);
    uart_puts(" stval=");
    uart_put_hex(sv39_save[7]);
    uart_puts(" resume-4=");
    uart_put_hex(sv39_save[5] - 4);
    uart_puts("\n");
    check(sv39_save[6] == 1, "unmapped load did not trap");
    check(sv39_save[2] == 13, "mcause != 13 (load page fault)");
    check(sv39_save[4] == va, "mtval != faulting VA");
    // Traps are taken in M-mode, so hardware writes mtval only; stval
    // must read back its reset value of zero, proving it is untouched.
    check(sv39_save[7] == 0, "stval unexpectedly nonzero");
    check(sv39_save[3] == sv39_save[5] - 4,
          "mepc != address of faulting ld");
}

// S-mode test phase: every load/store here is translated through the
// Sv39 tables. Entered via mret from main; never returns.
static void s_mode_tests(void) {
    unsigned long got, phys;

    uart_puts("s-mode: entered, translation active\n");

    // The walk, end to end: store and load through the mapped VA.
    got = test_mapped();
    uart_puts("mapped: stored 0xdeadbeefcafebabe via VA 0x40000000, loaded ");
    uart_put_hex(got);
    uart_puts(" seen=");
    uart_put_dec(sv39_save[6]);
    uart_puts("\n");
    check(sv39_save[6] == 0, "mapped access unexpectedly trapped");
    check(got == PATTERN, "mapped load returned wrong value");

    // The physical page must hold the pattern: the walk really landed
    // on the intended frame, not on some alias.
    phys = *(volatile unsigned long *)page;
    uart_puts("mapped: physical page now holds ");
    uart_put_hex(phys);
    uart_puts("\n");
    check(phys == PATTERN, "physical page != pattern after mapped store");

    // Fault path: invalid level-0 PTE, then invalid level-1 PTE, then
    // invalid root PTE.
    test_fault(FAULT_VA_L0, "fault-l0");
    test_fault(FAULT_VA_L1, "fault-l1");
    test_fault(FAULT_VA_L2, "fault-l2");

    if (fails == 0)
        uart_puts("RESULT: PASS (mapped round-trip ok, all faults mcause=13)\n");
    else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}

int main(void) {
    unsigned long root_ppn, l1t_ppn, l0t_ppn, l1u_ppn, l0u_ppn;
    unsigned long page_ppn, satp, ident_ppn;
    unsigned long uart_ppn;
    int i;

    uart_init();
    uart_puts("sv39-walk: Sv39 page-table walk and fault path (backlog item 39)\n");
    uart_puts("m-mode: mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    // Control: the data page is good RAM before any tables exist.
    for (i = 0; i < 8; i++)
        page[i] = (unsigned char)(0xA5 + i);
    for (i = 0; i < 8; i++)
        check(page[i] == (unsigned char)(0xA5 + i),
              "page buffer not writable before table setup");
    uart_puts("control: data page readable/writable before translation: ok\n");

    // The S-mode phase needs its code, stack, tables, and UART
    // reachable through the walk, so check they all sit inside the
    // identity window before mapping it.
    check(((unsigned long)root_pt & 0xFFFUL) == 0, "root_pt misaligned");
    check(((unsigned long)l1_test & 0xFFFUL) == 0, "l1_test misaligned");
    check(((unsigned long)l0_test & 0xFFFUL) == 0, "l0_test misaligned");
    check(((unsigned long)l1_uart & 0xFFFUL) == 0, "l1_uart misaligned");
    check(((unsigned long)l0_uart & 0xFFFUL) == 0, "l0_uart misaligned");
    check(((unsigned long)page & 0xFFFUL) == 0, "page misaligned");
    check((unsigned long)&_stack_top < IDENT_END, "stack outside identity window");
    check((unsigned long)s_mode_tests >= IDENT_BASE &&
          (unsigned long)s_mode_tests < IDENT_END,
          "s_mode_tests outside identity window");

    // Build the tables by hand.
    // root[2]: 1 GiB megapage leaf, identity [0x80000000, 0xC0000000).
    // root[1]: pointer (V only) to the level-1 table holding the test
    //   mapping; l1_test[0]: pointer to the level-0 table; l0_test[0]:
    //   leaf mapping the one 4 KiB data page.
    // root[0]: pointer to the level-1 table holding the UART mapping;
    //   l1_uart[128]: pointer to a level-0 table; l0_uart[0]: leaf
    //   mapping the UART MMIO page.
    // Every other entry in all five tables is zero (invalid).
    root_ppn = (unsigned long)root_pt >> 12;
    l1t_ppn = (unsigned long)l1_test >> 12;
    l0t_ppn = (unsigned long)l0_test >> 12;
    l1u_ppn = (unsigned long)l1_uart >> 12;
    l0u_ppn = (unsigned long)l0_uart >> 12;
    page_ppn = (unsigned long)page >> 12;
    ident_ppn = IDENT_BASE >> 12;
    uart_ppn = UART_BASE >> 12;
    root_pt[2] = (ident_ppn << 10) | (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);
    root_pt[1] = (l1t_ppn << 10) | PTE_V;
    root_pt[0] = (l1u_ppn << 10) | PTE_V;
    l1_test[0] = (l0t_ppn << 10) | PTE_V;
    l0_test[0] = (page_ppn << 10) | (PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);
    l1_uart[128] = (l0u_ppn << 10) | PTE_V;
    l0_uart[0] = (uart_ppn << 10) | (PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);

    uart_puts("table: VA 0x40000000 -> vpn2=1 vpn1=0 vpn0=0\n");
    print_pte("table: root_pt[1] ", root_pt[1]);
    print_pte("table: l1_test[0] ", l1_test[0]);
    print_pte("table: l0_test[0] ", l0_test[0]);
    print_pte("table: root_pt[2] ", root_pt[2]);
    print_pte("table: root_pt[0] ", root_pt[0]);
    print_pte("table: l1_uart[128]", l1_uart[128]);
    print_pte("table: l0_uart[0] ", l0_uart[0]);
    check((root_pt[1] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "root entry 1 is not a pure pointer (V only)");
    check((root_pt[0] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "root entry 0 is not a pure pointer (V only)");
    check((l1_test[0] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "l1 entry 0 is not a pure pointer (V only)");
    check((l1_uart[128] & (PTE_V | PTE_R | PTE_W | PTE_X)) == PTE_V,
          "uart l1 entry 128 is not a pure pointer (V only)");
    check((l0_test[0] & (PTE_V | PTE_R | PTE_W)) == (PTE_V | PTE_R | PTE_W),
          "leaf entry missing V/R/W");
    check((l0_test[0] & (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U)) == (PTE_V | PTE_R | PTE_W),
          "leaf entry has unexpected X/U bits");
    check(((root_pt[1] >> 10) & 0xFFFFFFFFFFFUL) == l1t_ppn,
          "root[1] PPN != l1_test page number");
    check(((l1_test[0] >> 10) & 0xFFFFFFFFFFFUL) == l0t_ppn,
          "l1_test[0] PPN != l0_test page number");
    check(((l0_test[0] >> 10) & 0xFFFFFFFFFFFUL) == page_ppn,
          "leaf PPN != page number");
    check(((l0_uart[0] >> 10) & 0xFFFFFFFFFFFUL) == uart_ppn,
          "uart leaf PPN != 0x10000");

    // PMP: the privileged spec says an S-mode (or U-mode) access fails
    // when no PMP entry matches, so the S-mode phase needs a grant.
    // Entry 1 is TOR [0, 0x80400000) with R|W|X: one entry covering the
    // identity-mapped code/data/stack, the page tables, and the UART
    // MMIO page. Unlocked entries are never checked for M-mode, so the
    // M-mode phase above is unaffected.
    __asm__ volatile("csrw pmpaddr0, zero");
    __asm__ volatile("csrw pmpaddr1, %0" :: "r"(0x20100000UL));
    __asm__ volatile("csrw pmpcfg0, %0" :: "r"(0x0F00UL));
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(satp));
    uart_puts("m-mode: pmpcfg0=");
    uart_put_hex(satp);
    uart_puts(" (entry1: TOR [0, 0x80400000) R|W|X)\n");
    check(satp == 0x0F00UL, "pmpcfg0 readback != 0x0F00");

    // Install the M-mode trap vector and arm mscratch, then enable Sv39.
    __asm__ volatile("csrw mtvec, %0" :: "r"(sv39_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(sv39_save));
    set_satp(SATP_MODE_SV39 | root_ppn);
    satp = read_satp();
    uart_puts("m-mode: satp=");
    uart_put_hex(satp);
    uart_puts(" (MODE=8 ASID=0)\n");
    check((satp >> 60) == 8, "satp MODE != 8 (Sv39)");
    check((satp & 0xFFFFFFFFFFFUL) == root_ppn, "satp PPN != root page");

    // Drop to S-mode: the test phase runs translated from here on.
    uart_puts("m-mode: entering S-mode\n");
    __asm__ volatile(
        "csrr t0, mstatus\n"
        "li t1, %0\n"          // MPP mask
        "not t1, t1\n"
        "and t0, t0, t1\n"     // MPP=00
        "li t1, %1\n"
        "or t0, t0, t1\n"      // MPP=01 (S)
        "csrw mstatus, t0\n"
        "csrw mepc, %2\n"      // s_mode_tests
        "mret\n"               // never returns
        :
        : "i"(MSTATUS_MPP_MASK), "i"(MSTATUS_MPP_S), "r"(s_mode_tests)
        : "t0", "t1", "memory");
    __builtin_unreachable();
}
