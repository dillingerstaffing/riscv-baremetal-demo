// mprv_main.c: mstatus.MPRV set/clear write/readback (backlog item
// riscv mstatus-mprv-readback).
//
// Mechanism under test: mstatus.MPRV (bit 17) is the M-mode bit that
// makes data loads and stores use the MPP privilege instead of the
// current mode, so the bit must read back as written. The whole
// experiment runs in M-mode on hart 0 (QEMU boots the ELF straight
// into M-mode with -bios none, so no privilege drop is needed). The
// program never issues a trapping instruction while MPRV is clear,
// and the minimal trap handler (mprv_trap.S) records
// mcause/mepc/mtval and a trap count, then parks the hart; reaching
// the printed PASS implies the trap count is zero, and the program
// also prints and checks the count explicitly. On PASS the machine
// shuts down through the virt test-device finisher (QEMU exits 0);
// on FAIL the hart parks in a wfi loop without touching the
// finisher.
//
// Hazard note: with MPRV=1 and MPP=U (the boot mstatus on this
// board has MPP=U, and no PMP entries are programmed), a data load
// or store is privilege-checked as a U-mode access and faults.
// The set/probe/clear sequence therefore runs inside ONE asm block
// that executes only CSR instructions between the csrs and the
// csrc, so no data memory access can happen while the bit is set.
// The two readbacks are moved to registers inside that block and
// stored to C variables only after the bit is clear again. This is
// not a side effect of the hardware; it is how the test stays
// inside its own verified surface (a trap would fail the run).

#include "../uart.h"

extern void mprv_trap_entry(void);

// Trap record, written by mprv_trap.S: [0] parked t1,
// [1] mcause, [2] mepc, [3] mtval, [4] trap count.
volatile unsigned long mprv_save[8];

#define MSTATUS_MPRV (1UL << 17)

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// FNV-1a (64-bit) over the verdict-relevant values in a fixed order:
// the boot mstatus, the set readback, the clear readback, and the
// post-run mstatus. No absolute addresses or live counters enter
// the checksum, so it is byte-identical across runs.
static unsigned long long cksum = 1469598103934665603ULL;

static void cks_feed(unsigned long v) {
    int i;
    for (i = 0; i < 8; i++) {
        cksum ^= (unsigned long long)((v >> (8 * i)) & 0xffUL);
        cksum *= 1099511628211ULL;
    }
}

static int fails = 0;
static int nchecks = 0;

static void check(int cond, const char *msg) {
    nchecks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long read_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

// Run the set/probe/clear sequence in one asm block. Only CSR
// instructions execute while MPRV is set: no loads or stores can
// occur between the csrs and the csrc (see the hazard note above).
// *set_rb receives the readback with the bit set, *clr_rb the
// readback after clearing.
static void mprv_probes(unsigned long mask, unsigned long *set_rb,
                        unsigned long *clr_rb) {
    __asm__ volatile("csrs mstatus, %2\n\t"
                     "csrr %0, mstatus\n\t"
                     "csrc mstatus, %2\n\t"
                     "csrr %1, mstatus\n\t"
                     : "=r"(*set_rb), "=r"(*clr_rb)
                     : "r"(mask)
                     : "memory");
}

int main(void) {
    unsigned long boot, set_rb, clr_rb, post, traps;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("mstatus.MPRV write/readback: set via csrs, clear via csrc\n");
    uart_puts("========================================\n\n");

    // Trap vector: any trap parks the hart (mprv_trap.S).
    __asm__ volatile("la t0, mprv_trap_entry\n\t"
                     "csrw mtvec, t0"
                     :
                     :
                     : "t0", "memory");

    // 1. Boot mstatus, before any write.
    boot = read_mstatus();
    uart_puts("boot: mstatus=");
    uart_put_hex(boot);
    uart_puts(" MPRV(bit17)=");
    uart_put_dec((boot >> 17) & 1UL);
    uart_puts(" MPP(bits12:11)=");
    uart_put_dec((boot >> 11) & 3UL);
    uart_puts("\n");
    cks_feed(boot);

    // 2. Set probe via csrs, clear via csrc, both in one
    // memory-free asm block. The set readback must carry bit 17
    // and no other unexpected bit; the clear readback must equal
    // the boot value exactly (the csrc is also the restore).
    mprv_probes(MSTATUS_MPRV, &set_rb, &clr_rb);
    uart_puts("set: csrs mstatus, bit17; readback=");
    uart_put_hex(set_rb);
    uart_puts("\n");
    check((set_rb & MSTATUS_MPRV) != 0,
          "MPRV set probe: bit 17 not set in the readback");
    check(set_rb == (boot | MSTATUS_MPRV),
          "MPRV set probe: readback != boot|MPRV (unexpected bits "
          "changed alongside bit 17)");
    cks_feed(set_rb);
    uart_puts("clear: csrc mstatus, bit17; readback=");
    uart_put_hex(clr_rb);
    uart_puts("\n");
    check(clr_rb == boot,
          "MPRV clear probe: readback != boot mstatus (bit did not "
          "clear exactly, or other bits changed)");
    cks_feed(clr_rb);

    // 3. Restore confirmation: read mstatus after the probes; it
    // must still equal the boot value, so the experiment left no
    // residue.
    post = read_mstatus();
    uart_puts("restore: mstatus=");
    uart_put_hex(post);
    uart_puts(" (boot was ");
    uart_put_hex(boot);
    uart_puts(")\n");
    check(post == boot,
          "restore: mstatus did not read back the boot value after "
          "the probes");
    cks_feed(post);

    // 4. Trap count: the handler parks on the first trap, so
    // reaching here already implies zero, but require it
    // explicitly as well.
    traps = mprv_save[4];
    uart_puts("traps recorded by the handler: ");
    uart_put_dec(traps);
    uart_puts("\n");
    check(traps == 0, "unexpected trap fired during the run");

    uart_puts("\nchecksum (FNV-1a over the readback values) = ");
    uart_put_hex(cksum);
    uart_puts("\n");
    uart_puts("checks: ");
    uart_put_dec((unsigned long)nchecks);
    uart_puts("  mismatches: ");
    uart_put_dec((unsigned long)fails);
    uart_puts("\n");

    uart_puts(fails == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;
    }
    for (;;) {
        __asm__ volatile("wfi");
    }
}
