// pmt_main.c: PMP TOR boundary test (backlog item 103).
//
// Programs two PMP entries in TOR (top-of-range) mode forming a single
// sharp boundary: entry 0 covers [0, scratch) with full permissions,
// entry 1 covers [scratch, scratch + 4 KiB) with no permissions and the
// lock bit set. The lowest-numbered matching entry decides, so the
// address exactly one byte below the boundary must be readable and the
// first byte of the denied region must trap.
//
// Verified in the M-mode trap handler (records mcause/mepc/mtval):
//   (a) an lbu of the last allowed byte (boundary - 1) completes with no
//       trap and returns the pattern byte written before programming;
//   (b) an lbu of the first denied byte (boundary) traps with mcause 5
//       (load access fault), mepc exactly the faulting instruction's
//       address, and mtval exactly the faulting address.
//
// The L (lock) bit on entry 1 is required: unlocked PMP entries are not
// checked against M-mode accesses, and this test runs in M-mode (QEMU
// boots the ELF straight into M-mode with -bios none). Locking is
// itself verified: after programming, a csrw pmpcfg0, 0 must leave
// entry 1's byte unchanged (0x88) while entry 0's unlocked byte clears
// to 0x00, matching the per-entry lock rule. The boundary behavior is
// then re-verified after that clear, proving entry 0's permissions were
// never load-bearing for the denial.
//
// Controls: the scratch buffer is written with a byte pattern and read
// back before any PMP programming (proving the address is good RAM), and
// a second buffer outside the region stays accessible after programming
// (proving the entry is narrow and the fault comes from the PMP check).

#include "../uart.h"

extern void pmt_trap_entry(void);

// 4 KiB scratch; its start is the tested boundary.
static volatile unsigned char scratch[4096] __attribute__((aligned(4096)));
// Probe buffer placed outside the PMP region (elsewhere in .bss).
static volatile unsigned long probe_outside[4];

// Trap save area, laid out for pmt_trap.S:
// [1]=loaded byte / saved t1 [2]=mcause [3]=mepc [4]=mtval
// [5]=resume pc [6]=seen flag. mscratch points here while armed.
volatile unsigned long pmt_save[7];

static void csr_write_pmpaddr0(unsigned long v) {
    __asm__ volatile("csrw pmpaddr0, %0" :: "r"(v));
}

static void csr_write_pmpaddr1(unsigned long v) {
    __asm__ volatile("csrw pmpaddr1, %0" :: "r"(v));
}

static void csr_write_pmpcfg0(unsigned long v) {
    __asm__ volatile("csrw pmpcfg0, %0" :: "r"(v));
}

static unsigned long csr_read_pmpcfg0(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(v));
    return v;
}

static unsigned long csr_read_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

// pmpcfg0 bytes: entry 0 = TOR (0x08) + R|W|X (0x07) = 0x0F, unlocked.
//                entry 1 = L (0x80) + TOR (0x08) + no perms = 0x88.
#define PMPCFG0_TOR_LAYOUT 0x880FUL
#define PMPCFG0_ENTRY1_LOCKED 0x88UL

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// Attempt an lbu at the given address; the trap handler resumes at
// label 1. One asm block so the layout is exact: the auipc is 4 bytes
// (never compressed), the lbu is 4 bytes (t1 is not a compressible
// register), then a 4-byte sd spills the loaded byte, then the resume
// label. So mepc of a faulting lbu must equal resume_pc - 8, checked
// below and confirmed against the disassembly (auipc at 0x8000026e,
// lbu at 0x80000272, sd at 0x80000276, resume at 0x8000027a).
// The resume address is taken inside the asm with `la t0, 1f`
// against a numeric local label, resolved exactly by the assembler
// (C &&label does not pin the position at -O2 without a computed goto).
static void test_lbu(unsigned long addr, int expect_trap, unsigned long expect_val) {
    __asm__ volatile(
        "la t0, 1f\n"
        "sd t0, 40(%0)\n"      // pmt_save[5]: resume pc
        "sd zero, 48(%0)\n"    // pmt_save[6]: seen = 0
        "auipc t0, 0\n"        // A: address of this auipc
        "lbu t1, 0(%1)\n"      // probed load, at A+4
        "sd t1, 8(%0)\n"       // pmt_save[1]: loaded byte (no-trap path)
        "1:\n"
        :
        : "r"(pmt_save), "r"(addr)
        : "t0", "t1", "memory");

    uart_puts("lbu @");
    uart_put_hex(addr);
    uart_puts(": seen=");
    uart_put_dec(pmt_save[6]);
    uart_puts(" mcause=");
    uart_put_hex(pmt_save[2]);
    uart_puts(" mepc=");
    uart_put_hex(pmt_save[3]);
    uart_puts(" mtval=");
    uart_put_hex(pmt_save[4]);
    uart_puts(" resume-8=");
    uart_put_hex(pmt_save[5] - 8);
    uart_puts(" byte=");
    uart_put_hex(pmt_save[1] & 0xFFUL);
    uart_puts("\n");

    if (expect_trap) {
        check(pmt_save[6] == 1, "expected trap did not happen");
        check(pmt_save[2] == 5, "mcause != 5 (load access fault)");
        check(pmt_save[3] == pmt_save[5] - 8,
              "mepc != address of faulting lbu");
        check(pmt_save[4] == addr, "mtval != faulting address");
    } else {
        check(pmt_save[6] == 0, "unexpected trap on allowed access");
        check((pmt_save[1] & 0xFFUL) == expect_val,
              "allowed byte read back wrong value");
    }
}

int main(void) {
    int i;
    unsigned long start, end, pmpaddr0, pmpaddr1, rb;

    uart_init();
    uart_puts("pmp-tor-boundary: PMP TOR boundary test\n");
    uart_puts("hart mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    start = (unsigned long)scratch;
    end = start + sizeof(scratch);
    check((start & 0xFFFUL) == 0, "scratch start not 4 KiB aligned");

    // Control 1: scratch is good RAM before any PMP programming.
    for (i = 0; i < 4096; i++)
        scratch[i] = (unsigned char)(i & 0xFF);
    for (i = 0; i < 4096; i++)
        check(scratch[i] == (unsigned char)(i & 0xFF),
              "scratch not writable before PMP programming");
    uart_puts("control: scratch readable/writable before PMP: ok\n");

    // The last allowed byte (start - 1) sits in BSS padding just below the
    // 4 KiB-aligned scratch; stamp a sentinel so the allowed-side probe
    // verifies a real value, not just the absence of a trap.
    *(volatile unsigned char *)(start - 1) = 0x5A;
    check(*(volatile unsigned char *)(start - 1) == 0x5A,
          "sentinel byte below boundary not writable");
    uart_puts("control: sentinel 0x5A at boundary-1: ok\n");

    // Install the trap vector (direct mode) and arm mscratch.
    __asm__ volatile("csrw mtvec, %0" :: "r"(pmt_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(pmt_save));

    // Entry 0: TOR over [0, start), full permissions (unlocked).
    // Entry 1: TOR over [start, end), no permissions, locked.
    // Lowest-numbered matching entry wins, so the boundary is exact.
    pmpaddr0 = start >> 2;
    pmpaddr1 = end >> 2;
    uart_puts("config: pmpaddr0=");
    uart_put_hex(pmpaddr0);
    uart_puts(" (TOR [0, ");
    uart_put_hex(start);
    uart_puts(") allow) pmpaddr1=");
    uart_put_hex(pmpaddr1);
    uart_puts(" (TOR [");
    uart_put_hex(start);
    uart_puts(", ");
    uart_put_hex(end);
    uart_puts(") deny,locked)\n");
    csr_write_pmpaddr0(pmpaddr0);
    csr_write_pmpaddr1(pmpaddr1);
    csr_write_pmpcfg0(PMPCFG0_TOR_LAYOUT);

    rb = csr_read_pmpcfg0();
    uart_puts("config: pmpcfg0 readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == PMPCFG0_TOR_LAYOUT, "pmpcfg0 readback != 0x880F");

    // The lock on entry 1 must hold: clearing pmpcfg0 leaves entry 1's
    // byte at 0x88, while entry 0's unlocked byte clears to 0x00 (the
    // lock rule is per entry; TOR also pins pmpaddr0, entry 1's lower
    // bound). The boundary test below runs after this clear, proving
    // entry 0's permissions were never load-bearing for the denial.
    csr_write_pmpcfg0(0);
    rb = csr_read_pmpcfg0();
    uart_puts("config: pmpcfg0 after clear attempt=");
    uart_put_hex(rb);
    uart_puts(" (entry1 byte 0x88 locked, entry0 byte cleared)\n");
    check((rb & 0xFF00UL) == (PMPCFG0_ENTRY1_LOCKED << 8),
          "locked entry1 pmpcfg byte was modified");
    check((rb & 0xFFUL) == 0,
          "unlocked entry0 pmpcfg byte did not clear");

    // Control 2: an address outside the region stays accessible.
    for (i = 0; i < 4; i++)
        probe_outside[i] = 0x1122334455667788UL + (unsigned long)i;
    for (i = 0; i < 4; i++)
        check(probe_outside[i] == 0x1122334455667788UL + (unsigned long)i,
              "address outside PMP region became inaccessible");
    uart_puts("control: address outside region still accessible: ok\n");

    // (a) Last allowed byte: no trap, sentinel 0x5A back.
    test_lbu(start - 1, 0, 0x5AUL);
    // (b) First denied byte: trap, mcause=5, exact mepc/mtval.
    test_lbu(start, 1, 0);

    if (fails == 0)
        uart_puts("RESULT: PASS (boundary-1 readable, boundary traps mcause=5)\n");
    else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
