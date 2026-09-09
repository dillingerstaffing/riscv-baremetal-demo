// pmp_main.c: PMP no-access denial test (backlog item 25).
//
// Programs PMP entry 0 as a locked NAPOT region with no permissions
// (R=W=X=0, L=1) covering a 4 KiB scratch buffer, then performs a load
// and a store inside the region. Each access must trap with the cause
// code the RISC-V privileged specification assigns: load access fault
// (mcause 5), store/AMO access fault (mcause 7). A minimal M-mode trap
// entry records mcause/mepc/mtval and resumes past the faulting
// instruction so both tests run in one boot.
//
// The L (lock) bit is required: unlocked PMP entries are not checked
// against M-mode accesses, and this test runs in M-mode (QEMU boots the
// ELF straight into M-mode with -bios none). Locking is itself verified
// by attempting to clear pmpcfg0 and reading it back.
//
// Controls: the scratch buffer is written and read back before the PMP
// entry is programmed (proving the address is good RAM), and a second
// buffer outside the region stays accessible after programming (proving
// the entry is narrow and the faults come from the PMP check).

#include "../uart.h"

extern void pmp_trap_entry(void);

// 4 KiB scratch, naturally aligned for a NAPOT region.
static volatile unsigned char scratch[4096] __attribute__((aligned(4096)));
// Probe buffer placed outside the PMP region (elsewhere in .bss).
static volatile unsigned long probe_outside[4];

// Trap save area, laid out for pmp_trap.S:
// [1]=t1 [2]=mcause [3]=mepc [4]=mtval [5]=resume pc [6]=seen flag.
// mscratch points here while a test is armed.
volatile unsigned long pmp_save[7];

static void csr_write_pmpaddr0(unsigned long v) {
    __asm__ volatile("csrw pmpaddr0, %0" :: "r"(v));
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

// PMP entry 0 config byte: L=1 (0x80), A=NAPOT (0x18), R=W=X=0.
#define PMPCFG0_ENTRY0 0x98UL

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// Attempt the load; the trap handler resumes at label 1.
// The whole sequence is one asm block so the layout is exact: the auipc
// is 4 bytes (never compressed), the lw is 4 bytes (t1/t2 are not
// compressible registers), and the resume label follows immediately.
// Therefore mepc of the faulting lw must equal resume_pc - 4, checked
// below. (An earlier version took the resume address with &&label, but
// this toolchain's -O2 miscompiles that into a wrong address; the
// in-asm local label avoids the issue entirely.)
static void test_load(void) {
    unsigned long saddr = (unsigned long)scratch;

    __asm__ volatile(
        "la t0, 1f\n"
        "sd t0, 40(%0)\n"      // pmp_save[5]: resume pc
        "sd zero, 48(%0)\n"    // pmp_save[6]: seen = 0
        "auipc t0, 0\n"        // A: address of this auipc
        "lw t1, 0(%1)\n"       // faulting load, at A+4
        "1:\n"
        :
        : "r"(pmp_save), "r"(saddr)
        : "t0", "t1", "memory");

    uart_puts("load:  seen=");
    uart_put_dec(pmp_save[6]);
    uart_puts(" mcause=");
    uart_put_hex(pmp_save[2]);
    uart_puts(" mepc=");
    uart_put_hex(pmp_save[3]);
    uart_puts(" mtval=");
    uart_put_hex(pmp_save[4]);
    uart_puts(" resume-4=");
    uart_put_hex(pmp_save[5] - 4);
    uart_puts("\n");
    check(pmp_save[6] == 1, "load did not trap");
    check(pmp_save[2] == 5, "load mcause != 5 (load access fault)");
    check(pmp_save[3] == pmp_save[5] - 4,
          "load mepc != address of faulting lw");
    check(pmp_save[4] == saddr, "load mtval != faulting address");
}

// Attempt the store; the trap handler resumes at label 1.
// Same exact-layout construction as test_load: mepc of the faulting sw
// must equal resume_pc - 4.
static void test_store(void) {
    unsigned long saddr = (unsigned long)scratch;
    unsigned int val = 0x5A5A5A5AU;

    __asm__ volatile(
        "la t0, 1f\n"
        "sd t0, 40(%0)\n"      // pmp_save[5]: resume pc
        "sd zero, 48(%0)\n"    // pmp_save[6]: seen = 0
        "auipc t0, 0\n"        // A: address of this auipc
        "sw %2, 0(%1)\n"       // faulting store, at A+4
        "1:\n"
        :
        : "r"(pmp_save), "r"(saddr), "r"(val)
        : "t0", "t1", "memory");

    uart_puts("store: seen=");
    uart_put_dec(pmp_save[6]);
    uart_puts(" mcause=");
    uart_put_hex(pmp_save[2]);
    uart_puts(" mepc=");
    uart_put_hex(pmp_save[3]);
    uart_puts(" mtval=");
    uart_put_hex(pmp_save[4]);
    uart_puts(" resume-4=");
    uart_put_hex(pmp_save[5] - 4);
    uart_puts("\n");
    check(pmp_save[6] == 1, "store did not trap");
    check(pmp_save[2] == 7, "store mcause != 7 (store/AMO access fault)");
    check(pmp_save[3] == pmp_save[5] - 4,
          "store mepc != address of faulting sw");
    check(pmp_save[4] == saddr, "store mtval != faulting address");
}

int main(void) {
    int i;
    unsigned long base, pmpaddr, rb;

    uart_init();
    uart_puts("pmp-denial: PMP no-access region test\n");
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
    __asm__ volatile("csrw mtvec, %0" :: "r"(pmp_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(pmp_save));

    // Program entry 0: locked NAPOT 4 KiB over scratch, no permissions.
    base = (unsigned long)scratch;
    check((base & 0xFFFUL) == 0, "scratch not 4 KiB aligned");
    pmpaddr = (base >> 2) | 0x1FFUL;  // NAPOT encoding for a 4 KiB region
    uart_puts("config: pmpaddr0=");
    uart_put_hex(pmpaddr);
    uart_puts(" (NAPOT 4 KiB at ");
    uart_put_hex(base);
    uart_puts(") pmpcfg0=0x98 (entry0: L=1 A=NAPOT R=W=X=0)\n");
    csr_write_pmpaddr0(pmpaddr);
    csr_write_pmpcfg0(PMPCFG0_ENTRY0);

    rb = csr_read_pmpcfg0();
    uart_puts("config: pmpcfg0 readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == PMPCFG0_ENTRY0, "pmpcfg0 readback != 0x98");

    // The lock bit must hold: clearing pmpcfg0 is ignored while L=1.
    csr_write_pmpcfg0(0);
    rb = csr_read_pmpcfg0();
    uart_puts("config: pmpcfg0 after clear attempt=");
    uart_put_hex(rb);
    uart_puts(" (lock holds)\n");
    check(rb == PMPCFG0_ENTRY0, "locked pmpcfg0 was modified");

    // Control 2: an address outside the region stays accessible.
    for (i = 0; i < 4; i++)
        probe_outside[i] = 0x1122334455667788UL + (unsigned long)i;
    for (i = 0; i < 4; i++)
        check(probe_outside[i] == 0x1122334455667788UL + (unsigned long)i,
              "address outside PMP region became inaccessible");
    uart_puts("control: address outside region still accessible: ok\n");

    test_load();
    test_store();

    if (fails == 0)
        uart_puts("RESULT: PASS (load mcause=5, store mcause=7)\n");
    else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
