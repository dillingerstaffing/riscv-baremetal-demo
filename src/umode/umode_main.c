// umode_main.c: M-mode to U-mode trap transition, one mechanism.
//
// The program installs an M-mode trap handler, then performs an mret
// with mstatus.MPP = 0 (U-mode) into a one-instruction U-mode payload
// (a single ecall). The handler records mcause/mepc/mtval and the
// mstatus.MPP it observed on entry, then returns to M-mode at
// m_report, which prints the trap registers and the verdict.
//
// What the checks mean:
//  - mcause == 8: the privileged spec assigns 8 to "environment call
//    from U-mode". The hardware derives this from the privilege mode
//    at trap time, so 8 (not 11, the M-mode ecall code) is the
//    independent evidence that the payload really ran in U-mode.
//  - mepc == payload_addr: the trap pc is exactly the payload's ecall
//    instruction. The address is taken with an in-asm numeric local
//    label ("la t0, 1f" / "1:"), never a C labels-as-values address.
//  - mstatus.MPP on trap entry == 0 (U): a second, independent record
//    of the mode the trap came from.
//  - mtval == 0: what the spec requires for ecall traps.
// Exactly one trap must fire; then RESULT: PASS.

#include "../uart.h"

extern void umode_trap_entry(void);
extern void m_report(void);

// Trap save area, mscratch points here: [0] t0, [1] t1, [2] mcause,
// [3] mepc, [4] mtval, [5] mstatus at entry.
volatile unsigned long umode_regs[8];

volatile unsigned long trap_mcause;
volatile unsigned long trap_mepc;
volatile unsigned long trap_mtval;
volatile unsigned long trap_mstatus;
volatile unsigned long trap_seen;
volatile unsigned long payload_addr;

// C dispatcher, called from umode_trap.S with r = umode_regs.
// Records the trap registers and returns the resume pc (m_report,
// which the asm entry resumes in M-mode with MPP=3).
unsigned long umode_handle(volatile unsigned long *r) {
    trap_mcause = r[2];
    trap_mepc = r[3];
    trap_mtval = r[4];
    trap_mstatus = r[5];
    trap_seen++;
    return (unsigned long)m_report;
}

// M-mode reporter, entered via mret from the trap handler. Prints the
// recorded trap registers, the payload address, and the verdict, then
// parks the hart.
void m_report(void) {
    int pass = 1;
    unsigned long entry_mpp;

    uart_puts("\n--- M-mode to U-mode trap report ---\n");
    uart_puts("trap: mcause=");
    uart_put_hex(trap_mcause);
    uart_puts(" mepc=");
    uart_put_hex(trap_mepc);
    uart_puts(" mtval=");
    uart_put_hex(trap_mtval);
    uart_puts("\n");
    uart_puts("payload ecall at ");
    uart_put_hex(payload_addr);
    uart_puts(", mstatus at trap entry ");
    uart_put_hex(trap_mstatus);
    uart_puts("\n");

    if (trap_seen != 1) {
        uart_puts("check failed: expected exactly 1 trap\n");
        pass = 0;
    }
    if (trap_mcause != 8) {
        uart_puts("check failed: mcause is not 8 (ecall from U-mode)\n");
        pass = 0;
    }
    if (trap_mepc != payload_addr) {
        uart_puts("check failed: mepc is not the payload ecall address\n");
        pass = 0;
    }
    if (trap_mtval != 0) {
        uart_puts("check failed: mtval is not 0\n");
        pass = 0;
    }
    entry_mpp = (trap_mstatus >> 11) & 3ul;
    if (entry_mpp != 0) {
        uart_puts("check failed: mstatus.MPP at trap entry is not U-mode\n");
        pass = 0;
    }
    if (pass)
        uart_puts("RESULT: PASS\n");
    else
        uart_puts("RESULT: FAIL\n");
    for (;;)
        __asm__ volatile("wfi");
}

int main(void) {
    uart_init();
    uart_puts("M-mode to U-mode trap transition\n");
    __asm__ volatile("csrw mtvec, %0" ::"r"(&umode_trap_entry));
    __asm__ volatile("csrw mscratch, %0" ::"r"(umode_regs));
    // PMP: with no PMP entry programmed, lower privilege modes have
    // access to no address at all (M-mode keeps full access). Open
    // the whole address space with one NAPOT R/W/X entry before the
    // drop; without this the first U-mode instruction fetch raises
    // an instruction access fault.
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t"  // A=NAPOT, R/W/X, unlocked
                     "csrw pmpcfg0, t0\n\t"
                     :
                     :
                     : "t0", "memory");
    uart_puts("dropping to U-mode: one-instruction ecall payload...\n");
    // Clear mstatus.MPP (bits 11-12) to select U-mode, point mepc at
    // the payload ecall, and mret. The payload address is taken with
    // an in-asm numeric local label ("la t0, 1f" / "1:") and stored to
    // the global by an explicit sd before mret: the block never falls
    // through, so an output-operand store the compiler would place
    // after the template would never execute. The .option norvc keeps
    // the ecall a 4-byte instruction so mepc is exact.
    __asm__ volatile(".option push\n\t"
                     ".option norvc\n\t"
                     "li t0, (3 << 11)\n\t"
                     "csrrc t0, mstatus, t0\n\t"  // MPP = 0 (U-mode)
                     "la t0, 1f\n\t"
                     "sd t0, %0\n\t"             // payload_addr = 1f
                     "csrw mepc, t0\n\t"
                     "mret\n"
                     "1:\n\t"
                     "ecall\n\t"
                     ".option pop\n\t"
                     : "=m"(payload_addr)
                     :
                     : "t0", "memory");
    // Unreachable: mret lands in the U-mode payload; its ecall traps
    // back to M-mode, and the handler resumes at m_report.
    for (;;)
        __asm__ volatile("wfi");
}
