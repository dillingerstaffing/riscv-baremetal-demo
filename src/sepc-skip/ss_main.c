// ss_main.c: sepc-resume-skip check (backlog item "riscv sepc-resume-skip").
//
// Mechanism under test: an S-mode trap handler that adds 4 to sepc
// before sret resumes the hart at the instruction immediately after a
// faulting instruction, skipping the fault exactly once. M-mode boot
// code delegates the supervisor load access fault to S-mode via
// medeleg bit 5, installs a direct-mode stvec handler, opens the
// whole address space with one PMP NAPOT R/W/X entry, then mrets with
// mstatus.MPP=01 into an S-mode payload. The payload executes a
// single 4-byte faulting load (`lw a0, 0(a1)` at an unmapped address,
// assembled under .option norvc so it is exactly 4 bytes); the S-mode
// handler records scause/stval/sepc-at-entry, adds 4 to sepc, records
// the adjusted sepc, and srets. The instruction at faulting+4 writes
// a marker word to memory; the program asserts the trap fired exactly
// once (a re-executed fault would trap again), the sepc delta is 4,
// the marker ran, and the faulting load never committed (a0 keeps its
// pre-fault sentinel).
//
// satp is Bare (read and asserted), so there is no page walk: the
// unmapped load raises a load access fault (scause=5), not a load
// page fault. The backlog item wrote "0xd" for the expected cause,
// assuming a page fault; the measured cause on QEMU 8.2.2 virt in
// Bare mode is 5, matching the M-mode sibling module
// (src/mepc-resume-skip, mcause=5 for the same unmapped address).
// medeleg delegates both bit 5 (load access fault) and bit 13 (load
// page fault); the readback is printed and the handler reports which
// cause actually fired.
//
// The faulting instruction's address and the resume address are taken
// with in-asm numeric local labels (`la reg, 2f` / `la reg, 3f`),
// never with C labels-as-values, per the documented 13.2.0 &&label
// miscompile. After the mret the rest of main() runs in S-mode and
// uses only memory and MMIO (UART, mtime, test finisher), no M-mode
// CSRs, so it cannot take a privilege fault.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down and
// QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status (124)
// in addition to the RESULT: FAIL line.

#include "../uart.h"

#define CLINT_MTIME   0x0200bff8UL  // 64-bit mtime, UART-drain timebase

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

#define SCAUSE_LOAD_ACCESS_FAULT 5UL
// Address with no mapping on the virt machine: `li a1, 0xC0000000`
// sign-extends on RV64, so the hart sees this full 64-bit value.
#define UNMAPPED_ADDR 0xFFFFFFFFC0000000UL
#define SENTINEL_A0   0xA0A0A0A0A0A0A0A0UL
#define RESUME_MARKER 0xDEADBEEFDEADBEEFUL
// lw a0, 0(a1): imm=0, rs1=a1(11), funct3=010 (word), rd=a0(10), op=0000011.
#define INSN_LW_A0_A1 0x0005A503UL

// medeleg bits delegated: 5 (supervisor load access fault) and 13
// (supervisor load page fault). Both stick on QEMU 8.2.2 virt (the
// run prints the readback and requires bit 5; the fault taken is
// reported by the handler, not assumed).
#define MEDELEG_LOAD_FAULTS ((1UL << 5) | (1UL << 13))

// Trap scratch: scause, stval, sepc at trap entry, trap counter, sepc
// after the handler's +4 adjust, one spare word. sscratch points
// here; boot.S clears BSS so the counter starts at 0.
static volatile unsigned long ss_regs[6];

// Marker slot: written by the instruction at faulting+4, i.e. only if
// the handler resumed exactly one instruction past the fault.
static volatile unsigned long ss_marker[1];

extern void ss_trap_entry(void);
extern void ss_mtrap_park(void);

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
    unsigned long scause, stval, sepc_before, sepc_after, count, insn;

    uart_init();
    uart_puts("sepc-resume-skip: S-mode handler adds 4 to sepc, fault skipped once\n");

    // M-mode setup. Everything in this block runs in M-mode; the drop
    // to S-mode happens in the payload asm block below.

    // Trap vectors: direct-mode stvec for the delegated S-mode fault,
    // sscratch pointing at the scratch area, and a park loop on mtvec
    // as a safety net (no M-mode trap is expected; any M-mode trap
    // parks the hart so the harness observes a timeout).
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)ss_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)ss_regs));
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)ss_mtrap_park));
    __asm__ volatile("csrci mstatus, 8");  // MIE clear: no M-mode interrupt
    {
        unsigned long tv, satp, mie;
        __asm__ volatile("csrr %0, stvec" : "=r"(tv));
        __asm__ volatile("csrr %0, satp" : "=r"(satp));
        __asm__ volatile("csrr %0, mie" : "=r"(mie));
        uart_puts("setup: stvec=");
        uart_put_hex(tv);
        uart_puts(" satp=");
        uart_put_hex(satp);
        uart_puts(" mie=");
        uart_put_hex(mie);
        uart_puts("\n");
        check((tv & ~3UL) == (unsigned long)ss_trap_entry,
              "stvec did not take the handler address");
        check((tv & 3UL) == 0, "stvec not in direct mode");
        check(satp == 0, "satp not Bare at boot");
        check(mie == 0, "mie nonzero at boot");
    }

    // Delegate supervisor load faults to S-mode via medeleg (bits 5
    // and 13). The run requires bit 5 set and prints the full
    // readback; which cause actually fires is measured by the
    // handler, not assumed here.
    __asm__ volatile("csrs medeleg, %0" :: "r"(MEDELEG_LOAD_FAULTS));
    {
        unsigned long med;
        __asm__ volatile("csrr %0, medeleg" : "=r"(med));
        uart_puts("setup: medeleg=");
        uart_put_hex(med);
        uart_puts("\n");
        check((med & (1UL << 5)) != 0,
              "medeleg bit 5 (load access fault) did not stick");
    }

    // PMP: with no PMP entry programmed, S-mode has no access to any
    // address (M-mode keeps full access, lower modes default-deny).
    // Open the whole address space with one NAPOT R/W/X entry before
    // the drop; without this the first S-mode instruction fetch raises
    // an instruction access fault.
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t"  // A=NAPOT, R/W/X, unlocked
                     "csrw pmpcfg0, t0\n\t"
                     :
                     :
                     : "t0", "memory");

    // sie is 0 at boot and sstatus.SIE is clear, so the faulting load
    // is the only trap this run can take. If any setup check failed,
    // park here in M-mode instead of dropping.
    if (fails != 0) {
        uart_puts("RESULT: FAIL (setup)\n");
        for (;;)
            __asm__ volatile("wfi");
    }

    uart_puts("setup complete; dropping to S-mode...\n");

    // One volatile block: M-mode writes mepc with the S-mode payload
    // entry, sets mstatus.MPP=01, and mrets. The S-mode payload then
    // captures the faulting instruction's address (label 2) and the
    // address of the instruction right after it (label 3) with in-asm
    // numeric local labels, loads the sentinel into a0 and the
    // unmapped address into a1, executes the single faulting load,
    // then runs the marker store at label 3. The marker store is the
    // first instruction after the fault; it runs only if the handler
    // resumed at faulting+4. The block's outputs travel back to C
    // after the sret resumes at label 3; the C code that follows runs
    // in S-mode and touches only memory and MMIO.
    __asm__ volatile(
        ".option push\n\t"
        ".option norvc\n\t"
        "la t0, 1f\n\t"            // t0 = S-mode payload entry
        "csrw mepc, t0\n\t"
        "csrr t1, mstatus\n\t"
        "li t2, 0x1800\n\t"        // clear MPP bits 12:11
        "not t2, t2\n\t"
        "and t1, t1, t2\n\t"
        "li t2, 0x800\n\t"         // MPP = 01 (S-mode)
        "or t1, t1, t2\n\t"
        "csrw mstatus, t1\n\t"
        "mret\n\t"                 // enter S-mode at 1f
        "1:\n\t"                   // S-mode payload entry
        "la %[fault], 2f\n\t"
        "la %[resume], 3f\n\t"
        "li a0, %[sentinel]\n\t"
        "li a1, %[bad]\n\t"
        "2: lw a0, 0(a1)\n\t"      // faulting load, delegated to S-mode
        "3: li t0, %[marker]\n\t"
        "sd t0, 0(%[slot])\n\t"
        "mv %[a0out], a0\n\t"
        "mv %[a1out], a1\n\t"
        ".option pop\n\t"
        : [fault] "=r" (fault_addr), [resume] "=r" (resume_addr),
          [a0out] "=r" (a0_after), [a1out] "=r" (a1_after)
        : [sentinel] "i" (SENTINEL_A0), [bad] "i" (UNMAPPED_ADDR),
          [marker] "i" (RESUME_MARKER), [slot] "r" (ss_marker)
        : "t0", "t1", "t2", "a0", "a1", "memory");

    // Hart is in S-mode from here on. Trap record from the handler.
    scause = ss_regs[0];
    stval = ss_regs[1];
    sepc_before = ss_regs[2];
    count = ss_regs[3];
    sepc_after = ss_regs[4];
    insn = *(volatile unsigned int *)sepc_before;

    uart_puts("addrs: fault=");
    uart_put_hex(fault_addr);
    uart_puts(" resume=");
    uart_put_hex(resume_addr);
    uart_puts(" delta=");
    uart_put_dec(resume_addr - fault_addr);
    uart_puts("\n");
    uart_puts("trap: count=");
    uart_put_dec(count);
    uart_puts(" scause=");
    uart_put_hex(scause);
    uart_puts(" stval=");
    uart_put_hex(stval);
    uart_puts("\n");
    uart_puts("sepc: before=");
    uart_put_hex(sepc_before);
    uart_puts(" after=");
    uart_put_hex(sepc_after);
    uart_puts(" delta=");
    uart_put_dec(sepc_after - sepc_before);
    uart_puts(" insn@sepc_before=");
    uart_put_hex(insn);
    uart_puts("\n");
    uart_puts("post: marker=");
    uart_put_hex(ss_marker[0]);
    uart_puts(" a0=");
    uart_put_hex(a0_after);
    uart_puts(" a1=");
    uart_put_hex(a1_after);
    uart_puts("\n");

    check(count == 1, "trap count != 1 (fault re-executed?)");
    check(scause == SCAUSE_LOAD_ACCESS_FAULT,
          "scause != 5 (load access fault)");
    check(stval == UNMAPPED_ADDR, "stval != unmapped address");
    check(sepc_before == fault_addr,
          "sepc at trap entry != captured fault address");
    check(sepc_after - sepc_before == 4,
          "handler sepc delta != 4");
    check(sepc_after == resume_addr,
          "adjusted sepc != captured resume address");
    check(resume_addr - fault_addr == 4,
          "resume - fault != 4 (faulting insn not 4 bytes?)");
    check(insn == INSN_LW_A0_A1,
          "instruction at fault address is not lw a0,0(a1)");
    check(ss_marker[0] == RESUME_MARKER,
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
