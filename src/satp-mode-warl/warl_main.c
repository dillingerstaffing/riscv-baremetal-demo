// warl_main.c: satp MODE WARL probe (Bare / Sv39 / Sv48 / Sv57 / reserved).
//
// One mechanism: the satp.MODE field is WARL on RV64. Writing a MODE
// value the hart does not implement leaves the register unchanged
// (the whole write takes no effect), so probing MODE values
// documents which translation schemes this hart implements. The
// discovery half runs in M-mode, where a MODE write that sticks
// cannot fault anything (M-mode accesses are never translated), and
// the translation half runs in S-mode, where MODE=8 (Sv39) is
// switched on with a hand-built table and proven by a RAM read
// through the active translation, then switched back off.
//
// The run:
//   M-mode (boot):
//   1. record the boot-time satp value twice (the record is stable);
//   2. install the M-mode trap handler (direct mode; records
//      mcause/mepc/mtval, bumps the trap counter, prints FAIL and
//      parks on any trap) and open the address space with one PMP
//      NAPOT R/W/X entry (S-mode is default-deny with no PMP entry
//      programmed);
//   3. build the two page-table leaves by hand and sanity-check the
//      layout: root[2] must be a 1 GiB megapage leaf mapping
//      [0x80000000, 0xC0000000) to itself with R/W/X and A|D set
//      (code, data, stack, the table itself); root[0] must be a
//      1 GiB megapage leaf mapping [0, 0x40000000) to itself with
//      R/W and A|D set (covers the UART at 0x10000000). Every other
//      entry is zero (invalid) because boot.S clears the BSS;
//   4. MODE discovery: for MODE in {0, 8, 9, 10, 15} with PPN=0 and
//      ASID=0, publish the prior value, the write, and the readback,
//      and report whether the MODE stuck or the write had no
//      effect. MODE=0 must read back identical; MODE=8 must read
//      back MODE field 8; MODE=15 (reserved) must leave satp equal
//      to the prior value; MODE=9/10 publish their outcome and must
//      be either a clean stick (readback == write) or a clean
//      no-effect (readback == prior);
//   5. restore satp to the boot value (checked), store the RAM
//      canary with translation off, and drop to S-mode via mret
//      with mstatus.MPP=01.
//   S-mode payload:
//   6. check satp still equals the boot record (translation off on
//      entry), re-verify the two table entries (the safety gate:
//      never enable translation without a valid page table);
//   7. MODE=8 (Sv39) with the table PPN: write, sfence.vma, read
//      back; the MODE field must read back 8. Then the canary is
//      read through the active translation and must match the
//      value stored with translation off (the walk resolved to the
//      intended frame). Then satp is written back to Bare and the
//      readback must be 0 again;
//   8. the trap counter must be 0 (reaching the completion marker
//      proves no trap fired, since the handler parks on any trap),
//      the final satp read must equal the boot record exactly, a
//      checks/mismatches summary and an FNV-1a digest of every
//      verdict-relevant value are printed, then the completion
//      marker and RESULT: PASS, then the machine is shut down via
//      the virt test-device finisher (QEMU exits 0). Any failed
//      check prints RESULT: FAIL and parks instead.

#include "../uart.h"

extern void warl_trap_entry(void);
extern char _stack_top;

// Written by warl_trap.S if any trap fires (none expected).
volatile unsigned long warl_traps;
volatile unsigned long warl_mcause;
volatile unsigned long warl_mepc;
volatile unsigned long warl_mtval;
// Trap save area; mscratch points here while the module runs.
unsigned long warl_save[32];

// Root page table: two 1 GiB megapage leaves, everything else zero
// (invalid). BSS-cleared by boot.S.
static unsigned long root_pt[512] __attribute__((aligned(4096)));

// RAM canary: written in M-mode with translation off, read back in
// S-mode through the active Sv39 translation.
static volatile unsigned long canary_word;
#define CANARY 0xC0FFEE11DEADBEEFUL

// Boot-time satp, recorded in M-mode before anything writes it.
static unsigned long boot_satp;

#define SATP_MODE_SHIFT 60
#define SATP_PPN_MASK  0xFFFFFFFFFFFUL // satp PPN field, bits [43:0]

// PTE flag bits (RISC-V privileged spec, Sv39 PTE format).
#define PTE_V 0x001UL
#define PTE_R 0x002UL
#define PTE_W 0x004UL
#define PTE_X 0x008UL
#define PTE_A 0x040UL
#define PTE_D 0x080UL

// Window the code/data megapage leaf identity-maps.
#define CODE_BASE 0x80000000UL
#define CODE_END  0xC0000000UL

// MODE discovery probe kinds.
#define EXPECT_IDENTITY 0          // readback must equal the write
#define EXPECT_MODE_FIELD 1        // MODE field must equal written mode
#define EXPECT_STICK_OR_NOEFFECT 2 // readback == write, or readback == prior
#define EXPECT_NOEFFECT 3          // readback must equal the prior value

static int checks = 0;
static int mismatches = 0;

// FNV-1a 64-bit over the verdict-relevant values, so the three runs
// can be compared for byte-identical verdict-relevant output.
static unsigned long long fnv = 14695981039346656037ULL;
static void digest64(unsigned long v) {
    for (int i = 0; i < 8; i++) {
        fnv ^= (unsigned long long)((v >> (i * 8)) & 0xffUL);
        fnv *= 1099511628211ULL;
    }
}

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        mismatches++;
    }
}

static unsigned long csr_read_satp(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, satp" : "=r"(v));
    return v;
}

static void csr_write_satp(unsigned long v) {
    __asm__ volatile("csrw satp, %0" ::"r"(v) : "memory");
    // A MODE change governs subsequent accesses only after the TLB
    // is flushed; sfence.vma after every satp write keeps the
    // translated and untranslated phases strictly separated.
    __asm__ volatile("sfence.vma" ::: "memory");
}

// One M-mode MODE discovery probe: print the header BEFORE the
// write (so a faulting write still leaves its probe identified in
// the log), then publish prior/write/readback, feed all three into
// the digest, and check the outcome per kind.
static void discover_mode(unsigned mode, int kind, const char *name) {
    unsigned long prior, w, rb;

    uart_puts("probe ");
    uart_puts(name);
    uart_puts(":\n");
    prior = csr_read_satp();
    w = ((unsigned long)mode << SATP_MODE_SHIFT); // PPN=0, ASID=0
    csr_write_satp(w);
    rb = csr_read_satp();

    uart_puts("  prior    = ");
    uart_put_hex(prior);
    uart_puts("\n  write    = ");
    uart_put_hex(w);
    uart_puts("\n  readback = ");
    uart_put_hex(rb);
    uart_puts("\n");
    digest64(prior);
    digest64(w);
    digest64(rb);

    switch (kind) {
    case EXPECT_IDENTITY:
        check(rb == w, "MODE write/readback not identity");
        uart_puts("  outcome: readback == write\n");
        break;
    case EXPECT_MODE_FIELD:
        check((rb >> SATP_MODE_SHIFT) == mode,
              "MODE field did not read back the written mode");
        uart_puts("  outcome: MODE field stuck at ");
        uart_put_dec(mode);
        uart_puts("\n");
        break;
    case EXPECT_STICK_OR_NOEFFECT:
        check(rb == w || rb == prior,
              "MODE probe neither stuck cleanly nor left satp unchanged");
        if (rb == w) {
            uart_puts("  outcome: STUCK (whole write took effect)\n");
        } else {
            uart_puts("  outcome: NO EFFECT (readback == prior)\n");
        }
        break;
    case EXPECT_NOEFFECT:
    default:
        check(rb == prior,
              "reserved MODE write changed satp (expected no effect)");
        uart_puts("  outcome: NO EFFECT (readback == prior)\n");
        break;
    }
    uart_puts("\n");
}

// The S-mode payload: the Sv39 translation proof runs here. Entered
// via mret with mstatus.MPP=01. Global (not static) because the only
// reference is the `la` in the inline asm in main, which the compiler
// cannot see.
void warl_smode_test(void) {
    unsigned long root_ppn = (unsigned long)root_pt >> 12;
    unsigned long w, rb;

    uart_puts("in S-mode; Sv39 translation proof begins\n\n");

    // Translation must be off on entry: the M-mode phase restored
    // satp to the boot record before the drop.
    rb = csr_read_satp();
    digest64(rb);
    uart_puts("satp on S-mode entry = ");
    uart_put_hex(rb);
    uart_puts(" (boot record = ");
    uart_put_hex(boot_satp);
    uart_puts(")\n");
    check(rb == boot_satp, "satp != boot record on S-mode entry");

    // Safety gate: re-verify the table before translation is ever
    // enabled.
    unsigned long e2 = root_pt[2];
    unsigned long e0 = root_pt[0];
    digest64(e2);
    digest64(e0);
    uart_puts("table: root_pt[2] = ");
    uart_put_hex(e2);
    uart_puts(" (expect PPN 0x80000, flags V|R|W|X|A|D)\n");
    uart_puts("table: root_pt[0] = ");
    uart_put_hex(e0);
    uart_puts(" (expect PPN 0x0, flags V|R|W|A|D)\n");
    check(e2 == ((0x80000UL << 10) |
                 (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D)),
          "root_pt[2] is not the expected 1 GiB identity leaf");
    check(e0 == (PTE_V | PTE_R | PTE_W | PTE_A | PTE_D),
          "root_pt[0] is not the expected 1 GiB UART-window leaf");

    // MODE=8 (Sv39) with the valid table PPN.
    w = (8UL << SATP_MODE_SHIFT) | (root_ppn & SATP_PPN_MASK);
    uart_puts("enable MODE=8 (Sv39), table PPN:\n  write = ");
    uart_put_hex(w);
    uart_puts("\n");
    csr_write_satp(w);
    rb = csr_read_satp();
    digest64(w);
    digest64(rb);
    uart_puts("  readback = ");
    uart_put_hex(rb);
    uart_puts("\n  MODE field = ");
    uart_put_dec(rb >> SATP_MODE_SHIFT);
    uart_puts("\n");
    check((rb >> SATP_MODE_SHIFT) == 8,
          "Sv39 MODE did not read back 8");

    // Through-translation RAM read: the canary was stored with
    // translation off; reading it now walks root[2] and must land on
    // the same frame.
    unsigned long got = canary_word;
    digest64(CANARY);
    digest64(got);
    uart_puts("through-translation read: VA = ");
    uart_put_hex((unsigned long)&canary_word);
    uart_puts(" got = ");
    uart_put_hex(got);
    uart_puts(" expected = ");
    uart_put_hex(CANARY);
    uart_puts("\n");
    check(got == CANARY,
          "through-translation read mismatched the canary");

    // Back to Bare.
    uart_puts("restore MODE=0 (Bare):\n");
    csr_write_satp(0);
    rb = csr_read_satp();
    digest64(rb);
    uart_puts("  readback = ");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == 0, "satp did not restore to Bare after Sv39");

    // Verdict.
    uart_puts("\ntraps observed = ");
    uart_put_dec(warl_traps);
    uart_puts("\n");
    digest64(warl_traps);
    check(warl_traps == 0, "trap fired during the run");

    rb = csr_read_satp();
    digest64(rb);
    uart_puts("final satp = ");
    uart_put_hex(rb);
    uart_puts(" (boot record = ");
    uart_put_hex(boot_satp);
    uart_puts(")\n");
    check(rb == boot_satp, "final satp != boot satp");

    uart_puts("\nchecks: ");
    uart_put_dec((unsigned long)checks);
    uart_puts(" mismatches: ");
    uart_put_dec((unsigned long)mismatches);
    uart_puts("\nchecksum: ");
    uart_put_hex(fnv);
    uart_puts("\n");

    uart_puts("\nCOMPLETION MARKER: satp-mode-warl run finished\n");
    if (mismatches == 0) {
        uart_puts("RESULT: PASS\n");
        __asm__ volatile("li t1, 0x100000\n\t"
                         "li t2, 0x5555\n\t"
                         "sw t2, 0(t1)\n\t"
                         :
                         :
                         : "t1", "t2", "memory");
        for (;;)
            __asm__ volatile("wfi"); // unreachable; safety net
    }
    uart_puts("RESULT: FAIL\n");
    for (;;)
        __asm__ volatile("wfi");
}

int main(void) {
    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("satp MODE WARL probe (Bare/Sv39/Sv48/Sv57/reserved)\n");
    uart_puts("========================================\n\n");

    // 1. Boot-time satp, read in M-mode before anything writes it;
    // the second read checks the record is stable.
    boot_satp = csr_read_satp();
    unsigned long boot2 = csr_read_satp();
    digest64(boot_satp);
    digest64(boot2);
    uart_puts("satp at boot (M-mode reads) = ");
    uart_put_hex(boot_satp);
    uart_puts(" / ");
    uart_put_hex(boot2);
    uart_puts("\n");
    check(boot_satp == boot2, "boot satp record not stable");

    // 2. Install the M-mode trap handler; any trap lands here, gets
    // recorded, and parks.
    __asm__ volatile("la t0, warl_trap_entry\n\t"
                     "csrw mtvec, t0\n\t"
                     "la t0, warl_save\n\t"
                     "csrw mscratch, t0\n\t"
                     :
                     :
                     : "t0", "memory");

    // PMP: with no PMP entry programmed, S-mode has no access to any
    // address. Open the whole address space with one NAPOT R/W/X
    // entry before the drop; without this the first S-mode access
    // raises an access fault.
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t" // A=NAPOT, R/W/X, unlocked
                     "csrw pmpcfg0, t0\n\t"
                     :
                     :
                     : "t0", "memory");

    // 3. Build the two megapage leaves by hand. root[2]: VA
    // [0x80000000, 0xC0000000) -> PA 0x80000000 (code, data, stack,
    // the table itself), R/W/X with A|D set. root[0]: VA [0,
    // 0x40000000) -> PA 0x0 (covers the UART at 0x10000000), R/W
    // with A|D set. Every other entry stays zero (invalid).
    root_pt[2] = (0x80000UL << 10) |
                 (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);
    root_pt[0] = (PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);

    // Sanity: the table is page-aligned and the code, stack, table,
    // and canary all sit inside the identity window, so the S-mode
    // phase keeps executing once translation is on.
    check(((unsigned long)root_pt & 0xFFFUL) == 0, "root_pt misaligned");
    check((unsigned long)&_stack_top < CODE_END,
          "stack outside identity window");
    check((unsigned long)warl_smode_test >= CODE_BASE &&
          (unsigned long)warl_smode_test < CODE_END,
          "warl_smode_test outside identity window");
    check((unsigned long)&canary_word >= CODE_BASE &&
          (unsigned long)&canary_word < CODE_END,
          "canary outside identity window");

    // 4. MODE discovery in M-mode: a MODE write that sticks cannot
    // fault here (M-mode accesses are never translated), so even
    // Sv48/Sv57 are safe to probe.
    uart_puts("\nM-mode: satp.MODE discovery (PPN=0, ASID=0)\n\n");
    discover_mode(0, EXPECT_IDENTITY, "MODE=0 (Bare)");
    discover_mode(8, EXPECT_MODE_FIELD, "MODE=8 (Sv39)");
    discover_mode(9, EXPECT_STICK_OR_NOEFFECT, "MODE=9 (Sv48)");
    discover_mode(10, EXPECT_STICK_OR_NOEFFECT, "MODE=10 (Sv57)");
    discover_mode(15, EXPECT_NOEFFECT, "MODE=15 (reserved)");

    // 5. Restore the boot satp (checked), store the canary with
    // translation off, and drop to S-mode for the translation
    // proof.
    csr_write_satp(boot_satp);
    unsigned long rb = csr_read_satp();
    digest64(rb);
    uart_puts("discovery done; satp restored to ");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == boot_satp, "satp not restored to boot value");

    canary_word = CANARY;

    uart_puts("setup complete; dropping to S-mode...\n");

    // mret with MPP=01 (S-mode) into warl_smode_test.
    __asm__ volatile("la t0, warl_smode_test\n\t"
                     "csrw mepc, t0\n\t"
                     "csrr t0, mstatus\n\t"
                     "li t1, 0x1800\n\t"   // clear MPP bits 12:11
                     "not t1, t1\n\t"
                     "and t0, t0, t1\n\t"
                     "li t1, 0x800\n\t"    // MPP = 01 (S-mode)
                     "or t0, t0, t1\n\t"
                     "csrw mstatus, t0\n\t"
                     "mret\n\t"
                     :
                     :
                     : "t0", "t1", "memory");

    // Unreachable: mret lands in the S-mode payload, which finishes
    // via the test-device finisher or parks on failure.
    for (;;)
        __asm__ volatile("wfi");
}
