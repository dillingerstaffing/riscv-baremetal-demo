// ebt_main.c: ebreak-tripwire check.
//
// Question under test: does a deliberately executed `ebreak` trap with
// mcause 3 (breakpoint) and mepc naming the exact address of the
// tripwire instruction, so a bare-metal program can plant a breakpoint
// as a tripwire on a can't-happen path and the trap handler reports
// exactly where it died? The program installs an M-mode trap handler
// and fires two tripwires: the 4-byte `ebreak` (encoding 0x00100073,
// assembled under .option norvc) and the 2-byte compressed `c.ebreak`
// (encoding 0x9002, the form `__builtin_trap()` emits at -O2). For each,
// it captures the tripwire address and the post-tripwire address with
// in-asm numeric local labels, stores the resume address into the save
// area before the tripwire, and then verifies the trap record: mcause
// == 3, mepc == the tripwire address, the encoding at mepc matches the
// expected form, the resume lands exactly past the instruction (4 bytes
// for ebreak, 2 for c.ebreak), and a marker written after the tripwire
// is observed, proving execution continued past the trap.
//
// The addresses are taken with in-asm numeric local labels (`la reg,
// 1f` / `la reg, 2f`), never with C labels-as-values. Each phase's
// captures, resume-address store, tripwire, and post-tripwire marker
// live in a single volatile asm block so the compiler emits nothing
// between them; the checked values travel to C only through the block's
// output operands and the handler's records.
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

#define MCAUSE_BREAKPOINT 3UL
// ebreak: funct12=000000000001, rs1=00000, funct3=000, rd=00000,
// opcode=1110011 (SYSTEM).
#define INSN_EBREAK 0x00100073UL
// c.ebreak: the 2-byte compressed breakpoint, the form
// __builtin_trap() emits at -O2 (verified by objdump, see PROOF.md).
#define INSN_C_EBREAK 0x9002UL
#define MARKER_A 0xEBBE4AUL
#define MARKER_B 0xEBBE4BUL

// Trap scratch. Records are 3 words each: [0] mcause, [1] mepc at trap
// entry, [2] resume address stored by the test before the tripwire.
// Words 0..2 are the ebreak record, 3..5 the c.ebreak record, [16] the
// trap counter, [18]/[19] park t1/t2 for the handler, the rest spare.
// mscratch points here; boot.S clears BSS so the counter starts at 0.
static volatile unsigned long ebt_regs[20];

extern void ebt_trap_entry(void);

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
    unsigned long brk_a, post_a, marker_a;
    unsigned long brk_b, post_b, marker_b;
    unsigned long mcause_a, mepc_a, resume_a, insn_a;
    unsigned long mcause_b, mepc_b, resume_b, insn_b;
    unsigned long count;

    uart_init();
    uart_puts("ebreak-tripwire: two planted tripwires, two named traps\n");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the scratch area. Interrupts stay off (mie must read 0 at
    // boot and mstatus.MIE is cleared explicitly) so the two tripwires
    // are the only traps the run can take.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)ebt_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)ebt_regs));
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
        check((tv & ~3UL) == (unsigned long)ebt_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
        check(mie == 0, "mie nonzero at boot");
    }

    // PHASE A: the 4-byte ebreak. One volatile block: capture the
    // tripwire address (label 1) and the post-tripwire address
    // (label 2), store the resume address into ebt_regs[2] before the
    // ebreak, execute the single ebreak, then write the marker after it.
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %[brk], 1f\n\t"
        "la %[post], 2f\n\t"
        "sd %[post], 16(%[save])\n\t"
        "1: ebreak\n\t"
        "2: li %[marker], %[markval]\n\t"
        ".option pop\n\t"
        : [brk] "=r" (brk_a), [post] "=r" (post_a),
          [marker] "=r" (marker_a)
        : [save] "r" (ebt_regs), [markval] "i" (MARKER_A)
        : "memory");

    // PHASE B: the 2-byte compressed c.ebreak, the form the compiler
    // emits for __builtin_trap(). Same shape; the resume address goes
    // into ebt_regs[5] (the second record).
    __asm__ volatile(
        ".option push\n\t"
        ".option rvc\n\t"
        "la %[brk], 3f\n\t"
        "la %[post], 4f\n\t"
        "sd %[post], 40(%[save])\n\t"
        "3: c.ebreak\n\t"
        "4: li %[marker], %[markval]\n\t"
        ".option pop\n\t"
        : [brk] "=r" (brk_b), [post] "=r" (post_b),
          [marker] "=r" (marker_b)
        : [save] "r" (ebt_regs), [markval] "i" (MARKER_B)
        : "memory");

    // Trap records from the handler.
    mcause_a = ebt_regs[0];
    mepc_a = ebt_regs[1];
    resume_a = ebt_regs[2];
    mcause_b = ebt_regs[3];
    mepc_b = ebt_regs[4];
    resume_b = ebt_regs[5];
    count = ebt_regs[16];
    insn_a = *(volatile unsigned int *)mepc_a;
    insn_b = *(volatile unsigned short *)mepc_b;

    uart_puts("phase A: tripwire=");
    uart_put_hex(brk_a);
    uart_puts(" post=");
    uart_put_hex(post_a);
    uart_puts(" delta=");
    uart_put_dec(post_a - brk_a);
    uart_puts("\n");
    uart_puts("phase A trap: mcause=");
    uart_put_hex(mcause_a);
    uart_puts(" mepc=");
    uart_put_hex(mepc_a);
    uart_puts(" insn=");
    uart_put_hex(insn_a);
    uart_puts("\n");
    uart_puts("phase B: tripwire=");
    uart_put_hex(brk_b);
    uart_puts(" post=");
    uart_put_hex(post_b);
    uart_puts(" delta=");
    uart_put_dec(post_b - brk_b);
    uart_puts("\n");
    uart_puts("phase B trap: mcause=");
    uart_put_hex(mcause_b);
    uart_puts(" mepc=");
    uart_put_hex(mepc_b);
    uart_puts(" insn=");
    uart_put_hex(insn_b);
    uart_puts("\n");
    uart_puts("resume: stored_a=");
    uart_put_hex(resume_a);
    uart_puts(" stored_b=");
    uart_put_hex(resume_b);
    uart_puts(" marker_a=");
    uart_put_hex(marker_a);
    uart_puts(" marker_b=");
    uart_put_hex(marker_b);
    uart_puts(" traps=");
    uart_put_dec(count);
    uart_puts("\n");

    check(count == 2, "trap count != 2: the tripwires were not the only traps");
    check(mcause_a == MCAUSE_BREAKPOINT,
          "phase A mcause != 3 (breakpoint)");
    check(mcause_b == MCAUSE_BREAKPOINT,
          "phase B mcause != 3 (breakpoint)");
    check(mepc_a == brk_a,
          "phase A mepc != captured ebreak address: the trap did not name the tripwire");
    check(mepc_b == brk_b,
          "phase B mepc != captured c.ebreak address: the trap did not name the tripwire");
    check(insn_a == INSN_EBREAK,
          "instruction at phase A mepc is not ebreak (0x00100073)");
    check(insn_b == INSN_C_EBREAK,
          "halfword at phase B mepc is not c.ebreak (0x9002)");
    check(post_a - brk_a == 4,
          "phase A post - tripwire != 4 (ebreak not 4 bytes?)");
    check(post_b - brk_b == 2,
          "phase B post - tripwire != 2 (c.ebreak not 2 bytes?)");
    check(resume_a == post_a,
          "phase A stored resume != post-tripwire address");
    check(resume_b == post_b,
          "phase B stored resume != post-tripwire address");
    check(marker_a == MARKER_A,
          "phase A marker missing: execution did not continue past the ebreak");
    check(marker_b == MARKER_B,
          "phase B marker missing: execution did not continue past the c.ebreak");

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
