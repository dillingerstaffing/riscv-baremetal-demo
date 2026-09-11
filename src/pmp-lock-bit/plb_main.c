// plb_main.c: PMP lock-bit persistence test (backlog item: riscv pmp-lock-bit).
//
// Programs PMP entry 0 as a locked NAPOT region with no permissions
// (L=1, A=NAPOT, R=W=X=0) covering a 4 KiB scratch page, then tests the
// two properties the RISC-V privileged specification gives the lock bit:
//  1. Writes to pmpcfg0 and pmpaddr0 are ignored while entry 0's L bit
//     is set. All-ones writes to both registers must read back unchanged
//     (pmpaddr0 exactly; entry 0's byte of pmpcfg0 exactly; the unlocked
//     entries 1-7 do accept the write and read back 0xFF, reported but
//     not checked).
//  2. The locked entry's permissions are enforced even in M-mode: a
//     load inside the region traps with mcause 5 (load access fault),
//     mepc pointing at the faulting instruction, mtval carrying the
//     faulting address.
//
// The L bit is what makes property 2 testable in M-mode at all: unlocked
// PMP entries are not checked against M-mode accesses. This module
// isolates the lock-bit mechanism; denial trap codes for loads and
// stores are covered by src/pmp/.
//
// A minimal M-mode trap entry (plb_trap.S) records mcause/mepc/mtval,
// increments a trap counter, and resumes at the address the C code
// stores in plb_save[5]. The faulting load is a single inline-asm block
// so the instruction layout is exact (see below).

#include "../uart.h"

extern void plb_trap_entry(void);

// 4 KiB scratch, naturally aligned for a NAPOT region.
static volatile unsigned char scratch[4096] __attribute__((aligned(4096)));
// Probe buffer placed outside the PMP region (elsewhere in .bss).
static volatile unsigned long probe_outside[4];

// Trap save area, laid out for plb_trap.S:
// [1]=t1 [2]=mcause [3]=mepc [4]=mtval [5]=resume pc [6]=trap counter.
// mscratch points here while the test is armed.
volatile unsigned long plb_save[7];

static void csr_write_pmpaddr0(unsigned long v) {
    __asm__ volatile("csrw pmpaddr0, %0" :: "r"(v));
}

static void csr_write_pmpcfg0(unsigned long v) {
    __asm__ volatile("csrw pmpcfg0, %0" :: "r"(v));
}

static unsigned long csr_read_pmpaddr0(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpaddr0" : "=r"(v));
    return v;
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

// PMP entry 0 config byte: L=1 (0x80), A=NAPOT (0x18), R=W=X=0.
#define PLB_ENTRY0 0x98UL
#define PLB_ALLONES (~0UL)

static unsigned long nchecks = 0;
static unsigned long fails = 0;

static void check(int cond, const char *msg) {
    nchecks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// The faulting load. One asm block so the layout is exact: the auipc is
// 4 bytes (never compressed), the lw is 4 bytes (t1 is not a compressible
// register), and the resume label follows immediately. Therefore mepc of
// the faulting lw must equal resume_pc - 4, checked below. The resume
// address is taken inside the asm block with `la t0, 1f` against a
// numeric local label, which the assembler resolves exactly; C
// labels-as-values (&&label) cannot pin this address at -O2 (the
// optimizer may move a label no computed goto can reach, and the
// address follows the label).
static void test_load(void) {
    unsigned long saddr = (unsigned long)scratch;

    __asm__ volatile(
        "la t0, 1f\n"
        "sd t0, 40(%0)\n"      // plb_save[5]: resume pc
        "sd zero, 48(%0)\n"    // plb_save[6]: trap counter = 0
        "auipc t0, 0\n"        // A: address of this auipc
        "lw t1, 0(%1)\n"       // faulting load, at A+4
        "1:\n"
        :
        : "r"(plb_save), "r"(saddr)
        : "t0", "t1", "memory");

    uart_puts("load:  traps=");
    uart_put_dec(plb_save[6]);
    uart_puts(" mcause=");
    uart_put_hex(plb_save[2]);
    uart_puts(" mepc=");
    uart_put_hex(plb_save[3]);
    uart_puts(" mtval=");
    uart_put_hex(plb_save[4]);
    uart_puts(" resume-4=");
    uart_put_hex(plb_save[5] - 4);
    uart_puts("\n");
    check(plb_save[6] == 1, "load did not trap exactly once");
    check(plb_save[2] == 5, "load mcause != 5 (load access fault)");
    check(plb_save[3] == plb_save[5] - 4,
          "load mepc != address of faulting lw");
    check(plb_save[4] == saddr, "load mtval != faulting address");
}

int main(void) {
    int i;
    unsigned long base, pmpaddr, rb;

    uart_init();
    uart_puts("pmp-lock-bit: PMP lock-bit persistence test\n");
    uart_puts("hart mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    // Control 1: scratch is good RAM before any PMP programming.
    for (i = 0; i < 4; i++)
        scratch[i] = (unsigned char)(0xA5 + i);
    for (i = 0; i < 4; i++)
        check(scratch[i] == (unsigned char)(0xA5 + i),
              "scratch not writable before PMP programming");
    uart_puts("control: scratch readable/writable before PMP: ok\n");

    // Install the trap vector (direct mode) and arm mscratch.
    __asm__ volatile("csrw mtvec, %0" :: "r"(plb_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(plb_save));

    // Program entry 0: locked NAPOT 4 KiB over scratch, no permissions.
    base = (unsigned long)scratch;
    check((base & 0xFFFUL) == 0, "scratch not 4 KiB aligned");
    pmpaddr = (base >> 2) | 0x1FFUL;  // NAPOT encoding for a 4 KiB region
    uart_puts("config: pmpaddr0=");
    uart_put_hex(pmpaddr);
    uart_puts(" (NAPOT 4 KiB at ");
    uart_put_hex(base);
    uart_puts(") pmpcfg0 entry0=0x98 (L=1 A=NAPOT R=W=X=0)\n");
    csr_write_pmpaddr0(pmpaddr);
    csr_write_pmpcfg0(PLB_ENTRY0);

    rb = csr_read_pmpaddr0();
    uart_puts("config: pmpaddr0 readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == pmpaddr, "pmpaddr0 readback != programmed value");

    rb = csr_read_pmpcfg0();
    uart_puts("config: pmpcfg0 readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == PLB_ENTRY0, "pmpcfg0 readback != 0x98");

    // Lock persistence, part 1: an all-ones write to pmpcfg0 must not
    // change entry 0's byte. Entries 1-7 are unlocked and do accept the
    // write (they read back 0xFF); only the locked entry is checked.
    csr_write_pmpcfg0(PLB_ALLONES);
    rb = csr_read_pmpcfg0();
    uart_puts("lock:   pmpcfg0 after all-ones write=");
    uart_put_hex(rb);
    uart_puts("\n");
    check((rb & 0xFFUL) == PLB_ENTRY0,
          "locked entry 0 config byte changed by all-ones write");
    uart_puts("lock:   entry0 byte unchanged (0x98); entries 1-7 read 0x");
    uart_put_hex((rb >> 8) & 0xFFUL);
    uart_puts(" (unlocked, write accepted)\n");

    // Lock persistence, part 2: an all-ones write to pmpaddr0 must not
    // change it, because pmpaddr0 belongs to locked entry 0.
    csr_write_pmpaddr0(PLB_ALLONES);
    rb = csr_read_pmpaddr0();
    uart_puts("lock:   pmpaddr0 after all-ones write=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == pmpaddr, "locked pmpaddr0 changed by all-ones write");

    // Control 2: an address outside the region stays accessible.
    for (i = 0; i < 4; i++)
        probe_outside[i] = 0x1122334455667788UL + (unsigned long)i;
    for (i = 0; i < 4; i++)
        check(probe_outside[i] == 0x1122334455667788UL + (unsigned long)i,
              "address outside PMP region became inaccessible");
    uart_puts("control: address outside region still accessible: ok\n");

    // The locked entry's permissions apply to M-mode: the load traps.
    test_load();

    uart_puts("checks=");
    uart_put_dec(nchecks);
    uart_puts(" fails=");
    uart_put_dec(fails);
    uart_puts("\n");
    if (fails == 0)
        uart_puts("RESULT: PASS (lock holds; load mcause=5)\n");
    else
        uart_puts("RESULT: FAIL\n");
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
