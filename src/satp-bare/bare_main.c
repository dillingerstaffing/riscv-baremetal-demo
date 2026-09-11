// bare_main.c: satp MODE=Bare write/readback with a nonzero PPN, and
// the proof that Bare means no address translation, in S-mode.
//
// One mechanism: the MODE field of the supervisor address-translation
// and protection register (satp) on RV64. MODE=0 (Bare) means the
// effective address of every supervisor load, store, and instruction
// fetch is the physical address itself: no translation. This module
// pins two consequences of that definition on the QEMU `virt` board:
//   1. writing satp with MODE=Bare and a nonzero PPN reads back with
//      the MODE field at 0 (the written Bare encoding);
//   2. a supervisor load/store of a known physical address, done
//      while satp holds that Bare+nonzero-PPN value, touches the
//      physical contents directly: the value M-mode stored at the
//      physical address is exactly what S-mode loads, and the value
//      S-mode stores is exactly what a physical readback returns.
//      Had translation been active, the load would have walked the
//      garbage PPN as a page-table root and faulted.
//
// The run, in S-mode (dropped from M-mode boot via mret with
// mstatus.MPP=01; PMP is opened first because S-mode is default-deny
// with no PMP entry programmed, and an M-mode trap handler is
// installed so any unexpected trap is recorded and parked):
//   1. M-mode writes a canary to a scratch physical word (16 MiB
//      above RAM base, well outside the image) and reads it back;
//   2. in S-mode, csrw satp with MODE=0, ASID=0, PPN nonzero; csrr
//      readback; print write/readback and check the MODE field is 0;
//   3. in S-mode, load the scratch word: it must equal the M-mode
//      canary (physical content visible with no translation);
//   4. in S-mode, store a second canary and load it back: it must
//      match (S-mode stores land on the physical address);
//   5. restore satp to 0, check the trap counter is 0 (reaching the
//      completion marker proves no trap fired, since the handler
//      parks on any trap), print the completion marker and
//      RESULT: PASS, then shut the machine down via the virt
//      test-device finisher (QEMU exits 0).
//      Any failed check prints RESULT: FAIL and parks instead.

#include "../uart.h"

extern void bare_trap_entry(void);

// Written by bare_trap.S if any trap fires (none expected).
volatile unsigned long bare_traps;
volatile unsigned long bare_mcause;
volatile unsigned long bare_mepc;
volatile unsigned long bare_mtval;
// Trap save area; mscratch points here while the module runs.
unsigned long bare_save[32];

#define MPP_MASK (3UL << 11)
#define MPP_S    (1UL << 11)

// satp field layout, RV64.
#define SATP_MODE_SHIFT 60
#define SATP_ASID_SHIFT 44
#define SATP_MODE_MASK  (0xfUL << SATP_MODE_SHIFT)
#define SATP_PPN_MASK   ((1UL << SATP_ASID_SHIFT) - 1UL)

// Scratch physical word: 16 MiB above RAM base on the virt board
// (128 MiB RAM at 0x80000000), far outside the linked image and the
// 16 KiB boot stack.
#define PHYS_WORD 0x81000000UL

#define CANARY1 0xBA5EBA1100000001UL  // written by M-mode
#define CANARY2 0xBA5EBA1100000002UL  // written by S-mode

// Nonzero PPN paired with MODE=Bare (ASID=0). Any 44-bit value with
// low bits clear would do; this one is visibly nonzero on the wire.
#define PPN_TEST 0x00000DEADBEA0UL

static int fails = 0;

static void check(int cond, const char *msg) {
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

static void csr_write_satp(unsigned long v) {
    __asm__ volatile("csrw satp, %0" ::"r"(v) : "memory");
}

// The S-mode payload: the whole satp-bare experiment runs here.
// Entered via mret with mstatus.MPP=01, so this function executes in
// S-mode. Global (not static) because the only reference is the `la`
// in the inline asm below, which the compiler cannot see.
void bare_smode_test(void) {
    uart_puts("in S-mode; satp MODE=Bare experiment begins\n\n");

    // 2. Write satp with MODE=Bare, ASID=0, PPN nonzero; read back.
    unsigned long write_val = PPN_TEST;
    csr_write_satp(write_val);
    unsigned long rb = csr_read_satp();
    unsigned long mode = (rb & SATP_MODE_MASK) >> SATP_MODE_SHIFT;
    unsigned long asid = (rb >> SATP_ASID_SHIFT) & 0xffffUL;
    unsigned long ppn = rb & SATP_PPN_MASK;

    uart_puts("satp write/readback (MODE=Bare, nonzero PPN):\n");
    uart_puts("  write            = ");
    uart_put_hex(write_val);
    uart_puts("\n");
    uart_puts("  readback         = ");
    uart_put_hex(rb);
    uart_puts("\n");
    uart_puts("  MODE field       = ");
    uart_put_dec(mode);
    uart_puts("\n");
    uart_puts("  ASID field       = ");
    uart_put_hex(asid);
    uart_puts("\n");
    uart_puts("  PPN field        = ");
    uart_put_hex(ppn);
    uart_puts("\n");
    check(mode == 0, "MODE field did not read back 0 after Bare+PPN write");
    check(asid == 0, "ASID field changed during Bare+PPN write");
    // On this machine the whole written value (MODE=0, ASID=0, the
    // nonzero PPN) reads back verbatim; the PPN is stored even
    // though Bare mode ignores it for translation.
    check(rb == write_val,
          "satp readback did not equal the written Bare+PPN value");

    // 3. Supervisor load of the scratch word, with satp still holding
    // Bare+nonzero-PPN: Bare means the address is physical, so this
    // must return the canary M-mode stored at the physical word.
    volatile unsigned long *word = (volatile unsigned long *)PHYS_WORD;
    unsigned long loaded = *word;
    uart_puts("\nphysical access under satp=Bare+PPN:\n");
    uart_puts("  S-mode load of physical word = ");
    uart_put_hex(loaded);
    uart_puts("  (M-mode canary = ");
    uart_put_hex(CANARY1);
    uart_puts(")");
    uart_puts(loaded == CANARY1 ? "  PASS\n" : "  FAIL\n");
    if (loaded != CANARY1) {
        uart_puts("    FAIL: S-mode did not see the M-mode physical canary\n");
        fails++;
    }

    // 4. Supervisor store and loadback of the same word: S-mode
    // stores must land on the physical address under Bare.
    *word = CANARY2;
    unsigned long round = *word;
    uart_puts("  S-mode store/loadback          = ");
    uart_put_hex(round);
    uart_puts(round == CANARY2 ? "  PASS\n" : "  FAIL\n");
    if (round != CANARY2) {
        uart_puts("    FAIL: S-mode store/loadback mismatch\n");
        fails++;
    }

    // Restore satp to 0 (Bare, ASID 0, PPN 0).
    csr_write_satp(0);
    check(csr_read_satp() == 0, "satp did not restore to 0");

    // 5. Trap count and verdict. The M-mode handler parks on any
    // trap, so reaching this point with the counter at 0 proves the
    // happy path took no traps.
    uart_puts("\ntraps observed = ");
    uart_put_dec(bare_traps);
    uart_puts("\n");
    check(bare_traps == 0, "trap fired during the satp-bare experiment");

    uart_puts("\nCOMPLETION MARKER: satp-bare run finished\n");
    if (fails == 0) {
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
    uart_puts("satp MODE=Bare write/readback + direct\n");
    uart_puts("physical access (S-mode)\n");
    uart_puts("========================================\n\n");

    // 1. Boot-time satp, read in M-mode before the drop.
    unsigned long boot_satp = csr_read_satp();
    uart_puts("satp at boot (M-mode read) = ");
    uart_put_hex(boot_satp);
    uart_puts("\n");

    // Write the M-mode canary to the scratch physical word and read
    // it back: this is the physical ground truth the S-mode payload
    // will be checked against.
    volatile unsigned long *word = (volatile unsigned long *)PHYS_WORD;
    *word = CANARY1;
    unsigned long mcheck = *word;
    uart_puts("M-mode canary stored at ");
    uart_put_hex(PHYS_WORD);
    uart_puts(" = ");
    uart_put_hex(mcheck);
    uart_puts(mcheck == CANARY1 ? "  PASS\n" : "  FAIL\n");
    if (mcheck != CANARY1) {
        uart_puts("RESULT: FAIL (M-mode canary did not stick)\n");
        for (;;)
            __asm__ volatile("wfi");
    }

    // Install the M-mode trap handler; any trap from the S-mode
    // payload lands here, gets recorded, and parks.
    __asm__ volatile("la t0, bare_trap_entry\n\t"
                     "csrw mtvec, t0\n\t"
                     "la t0, bare_save\n\t"
                     "csrw mscratch, t0\n\t"
                     :
                     :
                     : "t0", "memory");

    // PMP: with no PMP entry programmed, S-mode has no access to
    // any address. Open the whole address space with one NAPOT
    // R/W/X entry before the drop; without this the first S-mode
    // instruction fetch raises an instruction access fault.
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t" // A=NAPOT, R/W/X, unlocked
                     "csrw pmpcfg0, t0\n\t"
                     :
                     :
                     : "t0", "memory");

    uart_puts("setup complete; dropping to S-mode...\n");

    // mret with MPP=01 (S-mode) into bare_smode_test. The whole
    // experiment runs in S-mode from there.
    __asm__ volatile("la t0, bare_smode_test\n\t"
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
