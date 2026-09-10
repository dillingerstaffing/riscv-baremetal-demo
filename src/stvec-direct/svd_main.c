// svd_main.c: stvec direct-mode write/readback plus delegated S-mode
// trap delivery through the stvec base.
//
// One mechanism: the supervisor trap-vector base register (stvec)
// with MODE=00 (direct), and trap delivery to the stvec base for a
// medeleg-delegated S-mode environment call.
//
// Per run, in M-mode:
//   1. record the boot-time stvec value;
//   2. write stvec = <svd_trap_entry address> with the low two bits
//      clear (MODE=00), read it back, and check the mode bits read
//      back clear and the base matches the written address exactly;
//   3. set medeleg bit 9 (supervisor environment call) and verify the
//      bit reads back set, so the probe ecall traps to S-mode through
//      stvec instead of to M-mode through mtvec;
//   4. open the whole address space with one PMP NAPOT R/W/X entry
//      (S-mode is default-deny with no PMP entry programmed);
//   5. mret with mstatus.MPP=01 into an S-mode payload whose only
//      instruction is one ecall. The ecall address is captured with
//      `la t0, 1f`, an in-asm numeric local label, so the assembler
//      resolves the exact address (a C labels-as-values address is
//      misplaced at -O2); the payload ecall is wrapped in
//      .option norvc so it is exactly 4 bytes and mepc is exact.
//
// The S-mode handler (svd_trap.S) records scause, sepc and stval into
// globals, prints the trap record with the measured numbers, and
// prints RESULT: PASS only if every check holds: exactly 1 trap,
// scause == 9 (environment call from S-mode), sepc == the captured
// ecall address, stval == 0. On PASS the handler shuts the machine
// down via the virt test-device finisher (QEMU exits 0); on FAIL, or
// if any M-mode setup check fails, the hart parks instead.

#include "../uart.h"

extern void svd_trap_entry(void);

// Written by svd_trap.S on the run's single trap.
volatile unsigned long svd_traps;
volatile unsigned long svd_scause;
volatile unsigned long svd_sepc;
volatile unsigned long svd_stval;
// Written by the M-mode asm below, before the mret.
volatile unsigned long svd_ecall_addr;
// Trap save area; sscratch points here while the probe runs.
unsigned long svd_save[32];

#define MEDELEG_ECALL_S (1UL << 9)
#define MPP_MASK        (3UL << 11)
#define MPP_S           (1UL << 11)

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  SETUP FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long csr_read_stvec(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, stvec" : "=r"(v));
    return v;
}

static unsigned long csr_read_medeleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, medeleg" : "=r"(v));
    return v;
}

int main(void) {
    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("stvec direct-mode write/readback\n");
    uart_puts("+ delegated S-mode trap delivery\n");
    uart_puts("========================================\n\n");

    // 1. Boot-time stvec, then the direct-mode write/readback pair.
    unsigned long boot_stvec = csr_read_stvec();
    uart_puts("stvec at boot          = ");
    uart_put_hex(boot_stvec);
    uart_puts("\n");

    unsigned long handler = (unsigned long)&svd_trap_entry;
    uart_puts("handler base (svd_trap_entry) = ");
    uart_put_hex(handler);
    uart_puts("\n");
    check((handler & 3UL) == 0, "handler base not 4-byte aligned");

    // MODE=00: low two bits clear, so the written value is the bare base.
    __asm__ volatile("csrw stvec, %0" ::"r"(handler) : "memory");
    unsigned long stvec_rb = csr_read_stvec();
    uart_puts("stvec write            = ");
    uart_put_hex(handler);
    uart_puts("\n");
    uart_puts("stvec readback         = ");
    uart_put_hex(stvec_rb);
    uart_puts("\n");
    check((stvec_rb & 3UL) == 0, "stvec mode bits did not read back clear");
    check((stvec_rb & ~3UL) == handler,
          "stvec base did not read back the written address");

    // 2. Delegate the S-mode environment call (medeleg bit 9) so the
    // probe trap is taken through stvec rather than mtvec.
    unsigned long med_boot = csr_read_medeleg();
    uart_puts("medeleg at boot        = ");
    uart_put_hex(med_boot);
    uart_puts("\n");
    __asm__ volatile("csrs medeleg, %0" ::"r"(MEDELEG_ECALL_S) : "memory");
    unsigned long med_rb = csr_read_medeleg();
    uart_puts("medeleg after csrs 9   = ");
    uart_put_hex(med_rb);
    uart_puts("\n");
    check((med_rb & MEDELEG_ECALL_S) != 0,
          "medeleg bit 9 did not read back set");

    // 3. PMP: with no PMP entry programmed, S-mode has no access to
    // any address (M-mode keeps full access, lower modes
    // default-deny). Open the whole address space with one NAPOT
    // R/W/X entry before the drop; without this the first S-mode
    // instruction fetch raises an instruction access fault.
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t" // A=NAPOT, R/W/X, unlocked
                     "csrw pmpcfg0, t0\n\t"
                     :
                     :
                     : "t0", "memory");

    // 4. Point sscratch at the trap save area, then drop to S-mode.
    __asm__ volatile("csrw sscratch, %0" ::"r"(svd_save) : "memory");

    uart_puts("setup complete; dropping to S-mode at the ecall payload...\n");

    if (fails != 0) {
        uart_puts("RESULT: FAIL (setup)\n");
        for (;;)
            __asm__ volatile("wfi");
    }

    // mret with MPP=01 (S-mode) into the payload; the payload is a
    // single ecall at label 1, whose exact address is captured with
    // `la t0, 1f` (numeric asm local label, resolved exactly by the
    // assembler) and stored for the handler's sepc check. The ecall
    // is wrapped in .option norvc so it assembles to exactly 4 bytes
    // and the captured address is exact.
    __asm__ volatile(
        "la t0, 1f\n\t"
        "la t1, svd_ecall_addr\n\t"
        "sd t0, 0(t1)\n\t"     // record the ecall address for the handler
        "csrw mepc, t0\n\t"     // payload entry = the ecall itself
        "csrr t0, mstatus\n\t"
        "li t1, 0x1800\n\t"      // clear MPP bits 12:11
        "not t1, t1\n\t"
        "and t0, t0, t1\n\t"
        "li t1, 0x800\n\t"       // MPP = 01 (S-mode)
        "or t0, t0, t1\n\t"
        "csrw mstatus, t0\n\t"
        "mret\n\t"
        ".option push\n\t"
        ".option norvc\n\t"
        "1: ecall\n\t"
        ".option pop\n\t"
        "2:\n\t"                 // unreachable in a correct run
        "wfi\n\t"
        "j 2b\n\t"
        :
        :
        : "t0", "t1", "memory");

    // Unreachable: mret lands on the ecall (S-mode); the delegated
    // trap runs the S-mode handler, which prints the verdict.
    for (;;)
        __asm__ volatile("wfi");
}
