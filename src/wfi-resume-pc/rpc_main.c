// rpc_main.c: WFI resume-PC check (proof backlog item 87).
//
// Mechanism under test: a machine software interrupt trap taken with
// mepc pointing at a wfi instruction. The handler clears the interrupt
// source, advances the saved mepc past the wfi, and mret must resume
// execution at wfi+4 with every general-purpose register bit-identical
// to its pre-trap value.
//
// Per run:
//   1. x1-x31 are snapshotted into before[] by one volatile asm block.
//   2. The exact address of the wfi is captured with `la x5, 1f` (a
//      numeric asm local label; the assembler resolves it exactly,
//      unlike a C labels-as-values address, which riscv gcc 13.2.0
//      misplaces at -O2).
//   3. msip is already 1 (written with MIE clear, so no trap yet).
//      `csrsi mstatus, 8` enables MIE and the pending interrupt traps
//      immediately, so the trap's mepc is the wfi address and the wfi
//      itself never executes (verified: without the handler advancing
//      mepc, mret lands back on the wfi and the hart sleeps forever).
//   4. The asm handler (rpc_trap.S, no C call anywhere on the trap
//      path) records mcause/mepc, clears msip with a 32-bit store,
//      adds 4 to the saved mepc, restores all of x1-x31, and mretes.
//   5. Execution resumes at the label after wfi; its address is
//      captured as resume_pc, then x1-x31 are snapshotted into
//      after[].
//
// The C checks per run: exactly one trap, mcause is the machine
// software interrupt, the trap's mepc equals the wfi address, the
// resume pc equals wfi+4, msip reads 0 afterwards, and before[] and
// after[] agree on all 31 registers. Three runs; RESULT: PASS only if
// every check holds on every run.
//
// Exit: on PASS the virt test-device finisher shuts the machine down
// (QEMU exits 0); on FAIL the hart parks in a wfi loop and the bench
// harness observes the timeout exit status.

#include "../uart.h"

#define CLINT_MSIP0   0x02000000UL
#define MSTATUS_MIE   (1UL << 3)
#define MIE_MSIE      (1UL << 3)

// Interrupt bit plus exception code 3 = machine software interrupt.
#define MCAUSE_MSI    0x8000000000000003UL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

#define NRUNS 3

// Written by rpc_trap.S on every trap.
unsigned long rpc_mcause;
unsigned long rpc_mepc;
unsigned long rpc_traps;

extern void rpc_trap_entry(void);
extern unsigned long rpc_save[];

static volatile unsigned int *const msip0 = (volatile unsigned int *)CLINT_MSIP0;

static unsigned long before[31];   // x1..x31 snapshot, pre-trap
static unsigned long after[31];    // x1..x31 snapshot, post-resume
static unsigned long spill[2];     // t0/t1 spill slots for the asm block

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

// One run: snapshot x1-x31, capture the exact wfi address, enable MIE
// so the pending msip traps with mepc at the wfi, capture the resume
// pc after mret, snapshot x1-x31 again. The whole sequence is a single
// volatile asm block so the compiler emits nothing between the two
// snapshots. t0/t1 are listed as clobbered (so no operand pointer is
// allocated to them) and are also manually spilled/restored, so the
// snapshots see identical t0/t1 values on both sides.
__attribute__((noinline))
static void one_run(unsigned long *bp, unsigned long *ap,
                    unsigned long *wfi_addr, unsigned long *resume_pc,
                    unsigned long *sp) {
    __asm__ volatile(
        "sd x1, 0*8(%[before])\n\t"
        "sd x2, 1*8(%[before])\n\t"
        "sd x3, 2*8(%[before])\n\t"
        "sd x4, 3*8(%[before])\n\t"
        "sd x5, 4*8(%[before])\n\t"
        "sd x6, 5*8(%[before])\n\t"
        "sd x7, 6*8(%[before])\n\t"
        "sd x8, 7*8(%[before])\n\t"
        "sd x9, 8*8(%[before])\n\t"
        "sd x10, 9*8(%[before])\n\t"
        "sd x11, 10*8(%[before])\n\t"
        "sd x12, 11*8(%[before])\n\t"
        "sd x13, 12*8(%[before])\n\t"
        "sd x14, 13*8(%[before])\n\t"
        "sd x15, 14*8(%[before])\n\t"
        "sd x16, 15*8(%[before])\n\t"
        "sd x17, 16*8(%[before])\n\t"
        "sd x18, 17*8(%[before])\n\t"
        "sd x19, 18*8(%[before])\n\t"
        "sd x20, 19*8(%[before])\n\t"
        "sd x21, 20*8(%[before])\n\t"
        "sd x22, 21*8(%[before])\n\t"
        "sd x23, 22*8(%[before])\n\t"
        "sd x24, 23*8(%[before])\n\t"
        "sd x25, 24*8(%[before])\n\t"
        "sd x26, 25*8(%[before])\n\t"
        "sd x27, 26*8(%[before])\n\t"
        "sd x28, 27*8(%[before])\n\t"
        "sd x29, 28*8(%[before])\n\t"
        "sd x30, 29*8(%[before])\n\t"
        "sd x31, 30*8(%[before])\n\t"
        "sd x5, 0*8(%[spill])\n\t"
        "sd x6, 1*8(%[spill])\n\t"
        "la x5, 1f\n\t"            // exact wfi address, numeric local label
        "sd x5, 0(%[wfi_slot])\n\t"
        "ld x5, 0*8(%[spill])\n\t"
        "csrsi mstatus, 8\n\t"     // MIE=1: pending msip traps now
        "1: wfi\n\t"
        "2:\n\t"
        "la x6, 2b\n\t"            // resume pc: first insn after mret
        "sd x6, 0(%[resume_slot])\n\t"
        "ld x6, 1*8(%[spill])\n\t"
        "ld x5, 0*8(%[spill])\n\t"
        "sd x1, 0*8(%[after])\n\t"
        "sd x2, 1*8(%[after])\n\t"
        "sd x3, 2*8(%[after])\n\t"
        "sd x4, 3*8(%[after])\n\t"
        "sd x5, 4*8(%[after])\n\t"
        "sd x6, 5*8(%[after])\n\t"
        "sd x7, 6*8(%[after])\n\t"
        "sd x8, 7*8(%[after])\n\t"
        "sd x9, 8*8(%[after])\n\t"
        "sd x10, 9*8(%[after])\n\t"
        "sd x11, 10*8(%[after])\n\t"
        "sd x12, 11*8(%[after])\n\t"
        "sd x13, 12*8(%[after])\n\t"
        "sd x14, 13*8(%[after])\n\t"
        "sd x15, 14*8(%[after])\n\t"
        "sd x16, 15*8(%[after])\n\t"
        "sd x17, 16*8(%[after])\n\t"
        "sd x18, 17*8(%[after])\n\t"
        "sd x19, 18*8(%[after])\n\t"
        "sd x20, 19*8(%[after])\n\t"
        "sd x21, 20*8(%[after])\n\t"
        "sd x22, 21*8(%[after])\n\t"
        "sd x23, 22*8(%[after])\n\t"
        "sd x24, 23*8(%[after])\n\t"
        "sd x25, 24*8(%[after])\n\t"
        "sd x26, 25*8(%[after])\n\t"
        "sd x27, 26*8(%[after])\n\t"
        "sd x28, 27*8(%[after])\n\t"
        "sd x29, 28*8(%[after])\n\t"
        "sd x30, 29*8(%[after])\n\t"
        "sd x31, 30*8(%[after])\n\t"
        : // no outputs; everything goes through the pointers
        : [before]"r"(bp), [after]"r"(ap),
          [wfi_slot]"r"(wfi_addr), [resume_slot]"r"(resume_pc),
          [spill]"r"(sp)
        : "memory", "t0", "t1");
}

static const char *regname(int i) {
    static const char *names[31] = {
        "ra","sp","gp","tp","t0","t1","t2","s0","s1","a0","a1","a2",
        "a3","a4","a5","a6","a7","s2","s3","s4","s5","s6","s7","s8",
        "s9","s10","s11","t3","t4","t5","t6"
    };
    return names[i];
}

int main(void) {
    unsigned long wfi_addr, resume_pc, traps_before;
    int run, i, ndiff;

    uart_init();
    uart_puts("wfi-resume-pc: trap at wfi resumes at wfi+4, regs intact (item 87)\n");
    uart_puts("mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the save area. Enable only the machine software interrupt in
    // mie; mstatus.MIE stays clear until each run's asm block.
    __asm__ volatile("csrw mtvec, %0" :: "r"(rpc_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(rpc_save));
    {
        unsigned long tv;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        uart_puts("trap: mtvec=");
        uart_put_hex(tv);
        uart_puts(" mscratch=");
        uart_put_hex((unsigned long)rpc_save);
        uart_puts("\n");
        check((tv & ~3UL) == (unsigned long)rpc_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
    }
    __asm__ volatile("csrs mie, %0" :: "r"(MIE_MSIE));
    __asm__ volatile("csrc mstatus, %0" :: "r"(MSTATUS_MIE));

    // Control: with MIE set but msip clear, a quiet window must
    // produce no trap, proving the runs' traps come from the msip
    // write and not from spurious delivery.
    *msip0 = 0;
    __asm__ volatile("csrs mstatus, %0" :: "r"(MSTATUS_MIE));
    {
        unsigned long spins = 0;
        while (spins < 1000000UL)
            spins++;
    }
    __asm__ volatile("csrc mstatus, %0" :: "r"(MSTATUS_MIE));
    uart_puts("control: traps-with-msip-clear=");
    uart_put_dec(rpc_traps);
    uart_puts(" (expect 0)\n");
    check(rpc_traps == 0, "spurious trap with msip clear");

    for (run = 0; run < NRUNS; run++) {
        traps_before = rpc_traps;
        rpc_mcause = 0;
        rpc_mepc = 0;
        wfi_addr = 0;
        resume_pc = 0;

        // MIE is clear here, so the set cannot trap mid-sequence.
        *msip0 = 0;
        check(*msip0 == 0, "msip nonzero at run start");
        *msip0 = 1;
        check(*msip0 == 1, "msip readback != 1 after set");

        one_run(before, after, &wfi_addr, &resume_pc, spill);

        __asm__ volatile("csrc mstatus, %0" :: "r"(MSTATUS_MIE));

        uart_puts("run ");
        uart_put_dec((unsigned long)run);
        uart_puts(": wfi_addr=");
        uart_put_hex(wfi_addr);
        uart_puts(" trap_mepc=");
        uart_put_hex(rpc_mepc);
        uart_puts(" resume_pc=");
        uart_put_hex(resume_pc);
        uart_puts("\n");

        check(rpc_traps == traps_before + 1, "trap count did not advance by exactly 1");
        check(rpc_mcause == MCAUSE_MSI, "mcause != machine software interrupt");
        check(rpc_mepc == wfi_addr, "trap mepc != wfi address");
        check(resume_pc == wfi_addr + 4, "resume pc != wfi address + 4");
        check(rpc_mepc + 4 == resume_pc, "trap mepc + 4 != resume pc");
        check(*msip0 == 0, "msip not cleared by the trap handler");

        ndiff = 0;
        for (i = 0; i < 31; i++) {
            if (before[i] != after[i]) {
                if (ndiff < 4) {
                    uart_puts("  regdiff x");
                    uart_put_dec((unsigned long)(i + 1));
                    uart_puts(" (");
                    uart_puts(regname(i));
                    uart_puts("): before=");
                    uart_put_hex(before[i]);
                    uart_puts(" after=");
                    uart_put_hex(after[i]);
                    uart_puts("\n");
                }
                ndiff++;
            }
        }
        uart_puts("run ");
        uart_put_dec((unsigned long)run);
        uart_puts(": register diffs=");
        uart_put_dec((unsigned long)ndiff);
        uart_puts("/31\n");
        check(ndiff == 0, "a register changed across the trap");
    }

    __asm__ volatile("csrc mie, %0" :: "r"(MIE_MSIE));

    if (fails == 0) {
        uart_puts("RESULT: PASS (3 runs: resume==wfi+4, 0/31 register diffs each)\n");
        uart_puts("done\n");
        *VIRT_TEST_FINISHER = FINISHER_PASS;
    } else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
        uart_puts("done\n");
    }
    for (;;)
        __asm__ volatile("wfi");
}
