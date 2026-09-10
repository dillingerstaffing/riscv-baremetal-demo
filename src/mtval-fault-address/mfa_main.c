// mfa_main.c: mtval-fault-address check (backlog item 134,
// "riscv mtval-fault-address").
//
// Question under test: when the same unmapped address faults once on a
// load and once on a store, does mtval report the exact same faulting
// address for both traps (mcause 0x5 vs 0x7)? The program issues one
// `lw a0, 0(a1)` and one `sw a0, 0(a1)` at the same unmapped address,
// each in its own volatile asm block assembled under .option norvc so
// the faulting instruction is exactly 4 bytes. An M-mode trap handler
// records the mcause/mepc/mtval triple per trap into a two-record save
// area and resumes at a resume address each test stored before the
// fault, so the run proceeds past both faults without re-trapping.
//
// The faulting instruction's address and the resume address are taken
// with in-asm numeric local labels (`la reg, 1f` / `la reg, 2f`),
// never with C labels-as-values. Each test's fault, address captures,
// and resume-address store live in a single volatile asm block so the
// compiler emits nothing between them; the checked values travel to C
// only through the block's output operands and the handler's records.
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

#define MCAUSE_LOAD_ACCESS_FAULT  5UL
#define MCAUSE_STORE_ACCESS_FAULT 7UL
// Address with no mapping on the virt machine: `li a1, 0xC0000000`
// sign-extends on RV64, so the hart sees this full 64-bit value.
#define UNMAPPED_ADDR 0xFFFFFFFFC0000000UL
#define SENTINEL_A0   0xA0A0A0A0A0A0A0A0UL
// lw a0, 0(a1): imm=0, rs1=a1(11), funct3=010 (word), rd=a0(10), op=0000011.
#define INSN_LW_A0_A1 0x0005A503UL
// sw a0, 0(a1): imm=0, rs2=a0(10), rs1=a1(11), funct3=010 (word), op=0100011.
#define INSN_SW_A0_A1 0x00A5A023UL

// Trap scratch. Records 0..3 are the load fault's mcause/mtval/mepc/
// resume; records 4..7 are the store fault's. [8] is the trap counter,
// [9] and [10] park t1/t2 for the handler, [11] is spare. mscratch
// points here; boot.S clears BSS so the counter starts at 0.
static volatile unsigned long mfa_regs[12];

extern void mfa_trap_entry(void);

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
    unsigned long fault_load, resume_load, fault_store, resume_store;
    unsigned long a0_after_load, a1_after_load, a0_after_store, a1_after_store;
    unsigned long mcause_load, mtval_load, mepc_load;
    unsigned long mcause_store, mtval_store, mepc_store;
    unsigned long count, insn_load, insn_store;

    uart_init();
    uart_puts("mtval-fault-address: load vs store fault mtval agreement\n");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the scratch area. Interrupts stay off (mie must read 0 at
    // boot and mstatus.MIE is cleared explicitly) so the two faulting
    // accesses are the only traps the run can take.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mfa_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mfa_regs));
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
        check((tv & ~3UL) == (unsigned long)mfa_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
        check(mie == 0, "mie nonzero at boot");
    }

    // LOAD TEST. One volatile block: capture the faulting instruction's
    // address (label 1) and the resume address (label 2) with in-asm
    // numeric local labels, store the resume address into the load
    // record (mfa_regs[3]) before the fault, load the sentinel into a0
    // and the unmapped address into a1, execute the single faulting
    // load, then read a0/a1 back after resume.
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %[fault], 1f\n\t"
        "la %[resume], 2f\n\t"
        "sd %[resume], 24(%[save])\n\t"
        "li a0, %[sentinel]\n\t"
        "li a1, %[bad]\n\t"
        "1: lw a0, 0(a1)\n\t"
        "2: mv %[a0out], a0\n\t"
        "mv %[a1out], a1\n\t"
        ".option pop\n\t"
        : [fault] "=r" (fault_load), [resume] "=r" (resume_load),
          [a0out] "=r" (a0_after_load), [a1out] "=r" (a1_after_load)
        : [save] "r" (mfa_regs), [sentinel] "i" (SENTINEL_A0),
          [bad] "i" (UNMAPPED_ADDR)
        : "a0", "a1", "memory");

    // STORE TEST. Same shape at the same unmapped address, with the
    // resume address stored into the store record (mfa_regs[7]).
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la %[fault], 1f\n\t"
        "la %[resume], 2f\n\t"
        "sd %[resume], 56(%[save])\n\t"
        "li a0, %[sentinel]\n\t"
        "li a1, %[bad]\n\t"
        "1: sw a0, 0(a1)\n\t"
        "2: mv %[a0out], a0\n\t"
        "mv %[a1out], a1\n\t"
        ".option pop\n\t"
        : [fault] "=r" (fault_store), [resume] "=r" (resume_store),
          [a0out] "=r" (a0_after_store), [a1out] "=r" (a1_after_store)
        : [save] "r" (mfa_regs), [sentinel] "i" (SENTINEL_A0),
          [bad] "i" (UNMAPPED_ADDR)
        : "a0", "a1", "memory");

    // Trap records from the handler.
    mcause_load = mfa_regs[0];
    mtval_load = mfa_regs[1];
    mepc_load = mfa_regs[2];
    mcause_store = mfa_regs[4];
    mtval_store = mfa_regs[5];
    mepc_store = mfa_regs[6];
    count = mfa_regs[8];
    insn_load = *(volatile unsigned int *)mepc_load;
    insn_store = *(volatile unsigned int *)mepc_store;

    uart_puts("load:  fault=");
    uart_put_hex(fault_load);
    uart_puts(" resume=");
    uart_put_hex(resume_load);
    uart_puts(" delta=");
    uart_put_dec(resume_load - fault_load);
    uart_puts("\n");
    uart_puts("load trap:  mcause=");
    uart_put_hex(mcause_load);
    uart_puts(" mepc=");
    uart_put_hex(mepc_load);
    uart_puts(" mtval=");
    uart_put_hex(mtval_load);
    uart_puts(" insn=");
    uart_put_hex(insn_load);
    uart_puts("\n");
    uart_puts("store: fault=");
    uart_put_hex(fault_store);
    uart_puts(" resume=");
    uart_put_hex(resume_store);
    uart_puts(" delta=");
    uart_put_dec(resume_store - fault_store);
    uart_puts("\n");
    uart_puts("store trap: mcause=");
    uart_put_hex(mcause_store);
    uart_puts(" mepc=");
    uart_put_hex(mepc_store);
    uart_puts(" mtval=");
    uart_put_hex(mtval_store);
    uart_puts(" insn=");
    uart_put_hex(insn_store);
    uart_puts("\n");
    uart_puts("compare: mtval_load=");
    uart_put_hex(mtval_load);
    uart_puts(" mtval_store=");
    uart_put_hex(mtval_store);
    uart_puts(" equal=");
    uart_puts(mtval_load == mtval_store ? "yes" : "no");
    uart_puts(" unmapped=");
    uart_put_hex(UNMAPPED_ADDR);
    uart_puts("\n");
    uart_puts("post: a0_load=");
    uart_put_hex(a0_after_load);
    uart_puts(" a1_load=");
    uart_put_hex(a1_after_load);
    uart_puts(" a0_store=");
    uart_put_hex(a0_after_store);
    uart_puts(" a1_store=");
    uart_put_hex(a1_after_store);
    uart_puts("\n");

    check(count == 2, "trap count != 2");
    check(mcause_load == MCAUSE_LOAD_ACCESS_FAULT,
          "load mcause != 5 (load access fault)");
    check(mcause_store == MCAUSE_STORE_ACCESS_FAULT,
          "store mcause != 7 (store access fault)");
    check(mepc_load == fault_load,
          "load mepc != captured fault address");
    check(mepc_store == fault_store,
          "store mepc != captured fault address");
    check(resume_load - fault_load == 4,
          "load resume - fault != 4 (faulting insn not 4 bytes?)");
    check(resume_store - fault_store == 4,
          "store resume - fault != 4 (faulting insn not 4 bytes?)");
    check(insn_load == INSN_LW_A0_A1,
          "instruction at load fault address is not lw a0,0(a1)");
    check(insn_store == INSN_SW_A0_A1,
          "instruction at store fault address is not sw a0,0(a1)");
    check(mtval_load == UNMAPPED_ADDR,
          "load mtval != unmapped address");
    check(mtval_store == UNMAPPED_ADDR,
          "store mtval != unmapped address");
    check(mtval_load == mtval_store,
          "mtval differs between load and store fault");
    check(a0_after_load == SENTINEL_A0,
          "a0 changed: faulting load committed");
    check(a1_after_load == UNMAPPED_ADDR,
          "a1 changed: handler clobbered the faulting address (load)");
    check(a0_after_store == SENTINEL_A0,
          "a0 changed across the store fault");
    check(a1_after_store == UNMAPPED_ADDR,
          "a1 changed: handler clobbered the faulting address (store)");

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
