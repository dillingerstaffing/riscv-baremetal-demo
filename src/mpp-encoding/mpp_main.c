// mpp_main.c: mstatus.MPP encoding write/return/verify measurement
// (backlog item 107).
//
// Mechanism under test: the MPP field (bits 12:11) of mstatus and
// the mret privilege transition. For each of the four MPP
// encodings the module writes the encoding to the field, reads
// back what the CSR actually holds, executes mret with mepc at a
// one-instruction ecall payload, and lets the ecall trap back to
// the M-mode handler. The handler records mcause, mepc, mtval, and
// the mstatus value seen on trap entry, then returns to M-mode at
// phase_done, which prints the write/return/verify tuple and runs
// the checks before starting the next phase.
//
// The four phases, with the expected values the checks use:
//  - MPP=00 (U): ecall traps with mcause 8, entry MPP 0, mepc at
//    the payload address.
//  - MPP=01 (S): ecall traps with mcause 9, entry MPP 1, mepc at
//    the payload address.
//  - MPP=10 (reserved): measured on QEMU 8.2.2, the write is
//    coerced to 00 at write time (readback MPP = 0), so the phase
//    behaves as U: mcause 8, entry MPP 0. The check records the
//    coercion; that is the emulator's actual behavior, found by
//    probing before the module was written.
//  - MPP=11 (M): mret stays in M-mode, ecall traps with mcause
//    11, entry MPP 3, mepc at the payload address.
// mtval must be 0 on every ecall trap, and exactly one trap must
// fire per phase.
//
// The mcause code is the independent evidence of the landing
// mode: the hardware derives it from the privilege mode at trap
// time, so a failed drop would show 11 (M-mode ecall) instead of
// 8 or 9. The entry MPP bits are a second, independent record.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop
// without touching the finisher; the harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.
//
// The payload address is taken with an in-asm numeric local label
// ("la t0, 1f" / "1:") and stored to the global by an explicit sd
// before mret: the block never falls through, so an output-operand
// store the compiler would place after the template would never
// execute. The .option norvc keeps the ecall a 4-byte instruction
// so mepc is exact.

#include "../uart.h"

extern void mpp_trap_entry(void);
extern void phase_done(void);

#define MPP_SHIFT 11
#define MPP_MASK  (0x3UL << MPP_SHIFT)

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

#define NPHASES 4

// Trap save area, mscratch points here: [0] t0, [1] t1, [2] mcause,
// [3] mepc, [4] mtval, [5] mstatus at entry.
volatile unsigned long mpp_regs[8];

volatile unsigned long trap_mcause;
volatile unsigned long trap_mepc;
volatile unsigned long trap_mtval;
volatile unsigned long trap_mstatus;
volatile unsigned long trap_count;
volatile unsigned long payload_addr;
volatile unsigned long written_mpp;
volatile unsigned long readback_mpp;
volatile unsigned long phase;

static int fails = 0;

static const unsigned long phase_mpp[NPHASES] = {0, 1, 2, 3};
static const char *phase_name[NPHASES] = {"00 (U)", "01 (S)",
                                         "10 (reserved)", "11 (M)"};
// Expected trap values per phase. Phase 2's expectations (mcause 8,
// entry MPP 0) are the measured QEMU 8.2.2 behavior: the reserved
// write is coerced to U at write time.
static const unsigned long phase_mcause[NPHASES] = {8, 9, 8, 11};
static const unsigned long phase_entry_mpp[NPHASES] = {0, 1, 0, 3};

static unsigned long read_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static unsigned long read_mtime(void) {
    return *(volatile unsigned long *)0x0200bff8UL;
}

// Write the MPP field: clear bits 12:11, then set the encoding.
static void write_mpp(unsigned long v) {
    __asm__ volatile("li t0, (3 << 11)\n\t"
                     "csrrc t0, mstatus, t0\n\t"
                     "sll t0, %0, 11\n\t"
                     "csrs mstatus, t0\n\t"
                     ::"r"(v)
                     : "t0", "memory");
}

// Drop: mepc at the one-instruction ecall payload, mret. Control
// never returns here; the payload's ecall traps to the M-mode
// handler, which resumes at phase_done.
static void drop_to_payload(void) {
    __asm__ volatile(".option push\n\t"
                     ".option norvc\n\t"
                     "la t0, 1f\n\t"
                     "sd t0, %0\n\t"      // payload_addr = landing address
                     "csrw mepc, t0\n\t"
                     "mret\n"
                     "1:\n\t"
                     "ecall\n\t"
                     ".option pop\n\t"
                     : "=m"(payload_addr)
                     :
                     : "t0", "memory");
    // Unreachable: mret diverted control into the payload.
    for (;;)
        __asm__ volatile("wfi");
}

// C dispatcher, called from mpp_trap.S with r = mpp_regs.
// Records the trap registers and returns the resume pc
// (phase_done, which the asm entry resumes in M-mode with MPP=3).
unsigned long mpp_handle(volatile unsigned long *r) {
    trap_mcause = r[2];
    trap_mepc = r[3];
    trap_mtval = r[4];
    trap_mstatus = r[5];
    trap_count++;
    return (unsigned long)phase_done;
}

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  check failed: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static void run_phase(unsigned long p);

static void finish(void) {
    uart_puts(fails == 0 ? "\nRESULT: PASS\n" : "\nRESULT: FAIL\n");
    // Let the UART drain before touching the finisher device.
    {
        unsigned long drain = read_mtime();
        while (read_mtime() - drain < 100000UL)
            ;
    }
    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    // FAIL: park the hart without touching the finisher device. The
    // harness runs QEMU under timeout, so a FAIL is observable as
    // the timeout exit status (124) as well as the RESULT line.
    for (;;)
        __asm__ volatile("wfi");
}

// M-mode reporter, entered via mret from the trap handler after the
// payload's ecall. Prints the write/return/verify tuple for the
// phase that just trapped, runs the checks, then starts the next
// phase (or finishes after the last one).
void phase_done(void) {
    unsigned long entry_mpp = (trap_mstatus >> MPP_SHIFT) & 3UL;

    uart_puts("\n--- phase ");
    uart_put_dec(phase);
    uart_puts(": wrote MPP=");
    uart_puts(phase_name[phase]);
    uart_puts(" ---\n");
    uart_puts("write/return: wrote MPP=");
    uart_put_dec(written_mpp);
    uart_puts(", MPP readback=");
    uart_put_dec(readback_mpp);
    uart_puts("\n");
    uart_puts("landing (payload ecall) address: ");
    uart_put_hex(payload_addr);
    uart_puts("\n");
    uart_puts("trap: mcause=");
    uart_put_hex(trap_mcause);
    uart_puts(" mepc=");
    uart_put_hex(trap_mepc);
    uart_puts(" mtval=");
    uart_put_hex(trap_mtval);
    uart_puts(" entry MPP=");
    uart_put_dec(entry_mpp);
    uart_puts("\n");

    check(trap_count == 1, "expected exactly 1 trap in this phase");
    check(trap_mcause == phase_mcause[phase], "mcause is not the expected ecall code");
    check(trap_mepc == payload_addr, "mepc is not the payload ecall address");
    check(trap_mtval == 0, "mtval is not 0");
    check(entry_mpp == phase_entry_mpp[phase], "entry MPP is not the expected mode");
    if (phase == 2)
        check(readback_mpp == 0, "reserved MPP write was not coerced to 0 by the CSR");
    else
        check(readback_mpp == written_mpp, "MPP readback differs from the written value");

    phase++;
    if (phase < NPHASES) {
        run_phase(phase);
        // Unreachable: run_phase mrets into the next payload.
        for (;;)
            __asm__ volatile("wfi");
    }
    finish();
}

static void run_phase(unsigned long p) {
    phase = p;
    trap_count = 0;
    write_mpp(phase_mpp[p]);
    written_mpp = phase_mpp[p];
    readback_mpp = (read_mstatus() >> MPP_SHIFT) & 3UL;
    uart_puts("\nphase ");
    uart_put_dec(p);
    uart_puts(": MPP=");
    uart_puts(phase_name[p]);
    uart_puts(", write done, readback MPP=");
    uart_put_dec(readback_mpp);
    uart_puts("\n");
    drop_to_payload();
}

int main(void) {
    uart_init();
    uart_puts("mstatus.MPP encoding write/return/verify\n");
    __asm__ volatile("csrw mtvec, %0" ::"r"(&mpp_trap_entry));
    __asm__ volatile("csrw mscratch, %0" ::"r"(mpp_regs));
    // PMP: with no PMP entry programmed, lower privilege modes have
    // access to no address at all (M-mode keeps full access). Open
    // the whole address space with one NAPOT R/W/X entry before the
    // drops; without this the first U-mode or S-mode instruction
    // fetch raises an instruction access fault.
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t"  // A=NAPOT, R/W/X, unlocked
                     "csrw pmpcfg0, t0\n\t"
                     :
                     :
                     : "t0", "memory");
    run_phase(0);
    // Unreachable: the last phase's reporter calls finish().
    for (;;)
        __asm__ volatile("wfi");
}
