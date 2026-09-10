// mrs_main.c: mepc-resume-skip check (backlog item "riscv mepc-resume-skip").
//
// Mechanism under test: an M-mode trap handler that adds 4 to mepc
// before mret resumes the hart at the instruction immediately after a
// faulting instruction, skipping the fault exactly once. The program
// executes a 4-byte faulting load (`lw a0, 0(a1)` at an unmapped
// address, assembled under .option norvc so it is exactly 4 bytes),
// the handler records mcause/mtval/mepc-at-entry, adds 4 to mepc,
// records the adjusted mepc, and mrets. The instruction at
// faulting+4 writes a marker word to memory; the program asserts the
// marker ran, the trap fired exactly once (a re-executed fault would
// trap again), and the faulting load never committed (its destination
// register keeps its pre-fault sentinel).
//
// The faulting instruction's address and the resume address are taken
// with in-asm numeric local labels (`la reg, 1f` / `la reg, 2f`),
// never with C labels-as-values. The fault, the address captures, and
// the marker write live in a single volatile asm block so the compiler
// emits nothing between them; the checked values travel to C only
// through the block's output operands and the memory stores inside
// the block.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.

#include "../uart.h"

#define CLINT_MTIME   0x0200bff8UL  // 64-bit mtime, UART-drain timebase

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

#define MCAUSE_LOAD_ACCESS_FAULT 5UL
// Address with no mapping on the virt machine: `li a1, 0xC0000000`
// sign-extends on RV64, so the hart sees this full 64-bit value.
#define UNMAPPED_ADDR 0xFFFFFFFFC0000000UL
#define SENTINEL_A0   0xA0A0A0A0A0A0A0A0UL
#define RESUME_MARKER 0xDEADBEEFDEADBEEFUL
// lw a0, 0(a1): imm=0, rs1=a1(11), funct3=010 (word), rd=a0(10), op=0000011.
#define INSN_LW_A0_A1 0x0005A503UL

// Trap scratch: mcause, mtval, mepc at trap entry, trap counter, mepc
// after the handler's +4 adjust, one spare word. mscratch points
// here; boot.S clears BSS so the counter starts at 0.
static volatile unsigned long mrs_regs[6];

// Marker slot: written by the instruction at faulting+4, i.e. only if
// the handler resumed exactly one instruction past the fault.
static volatile unsigned long mrs_marker[1];

extern void mrs_trap_entry(void);

static unsigned long read_mtime(void) {
    return *(volatile unsigned long *)CLINT_MTIME;
}

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

int main(void) {
    unsigned long fault_addr, resume_addr, a0_after, a1_after;
    unsigned long mcause, mtval, mepc_before, mepc_after, count, insn;

    uart_init();
    uart_puts("mepc-resume-skip: handler adds 4 to mepc, fault skipped once\n");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the scratch area. Interrupts stay off (mie must read 0 at
    // boot and mstatus.MIE is cleared explicitly) so the faulting
    // load is the only trap the run can take.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mrs_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mrs_regs));
    __asm__ volatile("csrci mstatus, 8");
    {
        unsigned long tv, mie;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        __asm__ volatile("csrr %0, mie" : "=r"(mie));
        uart_puts("setup: mtvec=");
        uart_put_hex(tv);
        uart_puts(" mie=");
        uart_put_hex(mie);
        uart_puts("\n");
        check((tv & ~3UL) == (unsigned long)mrs_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
        check(mie == 0, "mie nonzero at boot");
    }

    // One volatile block: capture the faulting instruction's address
    // (label 1) and the address of the instruction right after it
    // (label 2) with in-asm numeric local labels, load the sentinel
    // into a0 and the unmapped address into a1, execute the single
    // faulting load, then run the marker store at label 2. The marker
    // store is the first instruction after the fault; it runs only
    // if the handler resumed at faulting+4.
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %[fault], 1f\n\t"
        "la %[resume], 2f\n\t"
        "li a0, %[sentinel]\n\t"
        "li a1, %[bad]\n\t"
        "1: lw a0, 0(a1)\n\t"
        "2: li t0, %[marker]\n\t"
        "sd t0, 0(%[slot])\n\t"
        "mv %[a0out], a0\n\t"
        "mv %[a1out], a1\n\t"
        ".option pop\n\t"
        : [fault] "=r" (fault_addr), [resume] "=r" (resume_addr),
          [a0out] "=r" (a0_after), [a1out] "=r" (a1_after)
        : [sentinel] "i" (SENTINEL_A0), [bad] "i" (UNMAPPED_ADDR),
          [marker] "i" (RESUME_MARKER), [slot] "r" (mrs_marker)
        : "a0", "a1", "t0", "memory");

    // Trap record from the handler.
    mcause = mrs_regs[0];
    mtval = mrs_regs[1];
    mepc_before = mrs_regs[2];
    count = mrs_regs[3];
    mepc_after = mrs_regs[4];
    insn = *(volatile unsigned int *)mepc_before;

    uart_puts("addrs: fault=");
    uart_put_hex(fault_addr);
    uart_puts(" resume=");
    uart_put_hex(resume_addr);
    uart_puts(" delta=");
    uart_put_dec(resume_addr - fault_addr);
    uart_puts("\n");
    uart_puts("trap: count=");
    uart_put_dec(count);
    uart_puts(" mcause=");
    uart_put_hex(mcause);
    uart_puts(" mtval=");
    uart_put_hex(mtval);
    uart_puts("\n");
    uart_puts("mepc: before=");
    uart_put_hex(mepc_before);
    uart_puts(" after=");
    uart_put_hex(mepc_after);
    uart_puts(" delta=");
    uart_put_dec(mepc_after - mepc_before);
    uart_puts(" insn@mepc_before=");
    uart_put_hex(insn);
    uart_puts("\n");
    uart_puts("post: marker=");
    uart_put_hex(mrs_marker[0]);
    uart_puts(" a0=");
    uart_put_hex(a0_after);
    uart_puts(" a1=");
    uart_put_hex(a1_after);
    uart_puts("\n");

    check(count == 1, "trap count != 1 (fault re-executed?)");
    check(mcause == MCAUSE_LOAD_ACCESS_FAULT,
          "mcause != 5 (load access fault)");
    check(mtval == UNMAPPED_ADDR, "mtval != unmapped address");
    check(mepc_before == fault_addr,
          "mepc at trap entry != captured fault address");
    check(mepc_after - mepc_before == 4,
          "handler mepc delta != 4");
    check(mepc_after == resume_addr,
          "adjusted mepc != captured resume address");
    check(resume_addr - fault_addr == 4,
          "resume - fault != 4 (faulting insn not 4 bytes?)");
    check(insn == INSN_LW_A0_A1,
          "instruction at fault address is not lw a0,0(a1)");
    check(mrs_marker[0] == RESUME_MARKER,
          "marker at faulting+4 did not run");
    check(a0_after == SENTINEL_A0,
          "a0 changed: faulting load committed");
    check(a1_after == UNMAPPED_ADDR,
          "a1 changed: handler clobbered the faulting address");

    uart_puts("RESULT: ");
    uart_puts(fails == 0 ? "PASS" : "FAIL");
    uart_puts(" (traps=");
    uart_put_dec(count);
    uart_puts(")\n");

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
