// sfo_main.c: FP access with mstatus.FS=Off must trap as
// illegal-instruction, and the same instruction with FS=Initial
// must execute (backlog item "riscv sstatus-fs-off-fp-trap").
//
// Mechanism under test: the FS field (bits 14:13) of mstatus is
// the gate on floating-point state. With FS=Off, attempting to
// execute any FP instruction must raise an illegal-instruction
// exception (mcause 0x2, mepc at the FP instruction). With
// FS=Initial the same instruction must execute: the run does
// fadd.d f0,f1,f2 on the loaded operands 1.5 and 2.25 and checks
// f0 reads back the exact double 3.75 (0x400e000000000000),
// proving the FPU ran rather than trapping.
//
// The run is straight M-mode code on QEMU 8.2.2 (virt, -bios
// none, so boot is M-mode on hart 0). Interrupts stay masked
// (mstatus.MIE clear, mie reads 0 at boot), so the only
// synchronous trap possible is the expected illegal-instruction
// from the fault phase. The M-mode handler (sfo_trap.S) records
// mcause/mepc/mtval, bumps a counter, advances mepc by 4 past
// the 4-byte fadd.d, and mrets. The fault site's address is
// taken with an in-asm numeric local label (la t0, 1f), never a
// C labels-as-values address. The FP instructions are emitted
// under .option arch,+d because the module builds with
// -march=rv64imac_zicsr; the compiler cannot allocate FP
// registers, so no clobbers are needed. At the end the boot
// mstatus word is restored exactly and the machine shuts down
// through the virt test-device finisher (QEMU exits 0) on PASS;
// on FAIL the hart parks in a wfi loop without touching the
// finisher, so under the harness's `timeout` a FAIL is the
// timeout exit status (124) plus the RESULT line.

#include "../uart.h"

#define FS_MASK    (0x3UL << 13)   // mstatus bits 14:13
#define FS_DIRTY   3UL
#define MCAUSE_ILLEGAL_INST 0x2UL
#define MISA_FD    ((1UL << 5) | (1UL << 3))  // misa bits F (5) and D (3)

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// Double-precision bit patterns, written as integers so the
// compiler's integer-only march never sees floating point.
#define DBL_1_5  0x3FF8000000000000UL  // 1.5
#define DBL_2_25 0x4002000000000000UL  // 2.25
#define DBL_3_75 0x400E000000000000UL  // 1.5 + 2.25

// Trap record, written by sfo_trap.S: [0] mcause, [1] mepc,
// [2] mtval, [3] trap counter. boot.S clears BSS so all start 0.
static volatile unsigned long sfo_trap[4];

// Address of the fadd.d that is expected to fault, recorded by
// the in-asm label itself.
static volatile unsigned long fault_site;

extern void sfo_trap_entry(void);

static unsigned long read_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static unsigned long read_misa(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, misa" : "=r"(v));
    return v;
}

static int fs_of(unsigned long v) {
    return (int)((v >> 13) & 3UL);
}

static unsigned long fnv1a64(const unsigned char *data, unsigned long len) {
    unsigned long h = 1469598103934665603UL;
    unsigned long i;
    for (i = 0; i < len; i++) {
        h ^= (unsigned long)data[i];
        h *= 1099511628211UL;
    }
    return h;
}

static int fails = 0;
static int checks = 0;

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

int main(void) {
    unsigned long tv, mie, misa, baseline, fs_off, fs_after_trap;
    unsigned long fs_after_control, restored;
    unsigned long b1 = DBL_1_5, b2 = DBL_2_25, f0bits = 0;
    unsigned long words[10], checksum;

    uart_init();
    uart_puts("sstatus-fs-off-fp-trap: FP with mstatus.FS=Off must trap illegal-instruction\n");

    // Trap vector: recording M-mode handler (sfo_trap.S).
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)sfo_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)sfo_trap));
    __asm__ volatile("csrci mstatus, 8");  // MIE clear: no M-mode interrupt
    __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    uart_puts("setup: mtvec=");
    uart_put_hex(tv);
    uart_puts(" mie=");
    uart_put_hex(mie);
    uart_puts("\n");
    check((tv & ~3UL) == (unsigned long)sfo_trap_entry,
          "mtvec did not take the handler address");
    check((tv & 3UL) == 0, "mtvec not in direct mode");
    check(mie == 0, "mie nonzero at boot");

    // fadd.d is a D-extension instruction; require misa to carry F
    // and D before relying on one.
    misa = read_misa();
    uart_puts("setup: misa=");
    uart_put_hex(misa);
    uart_puts("\n");
    check((misa & MISA_FD) == MISA_FD,
          "misa lacks the F/D extension bits (fadd.d needs D)");

    // 1. Boot baseline: FS must read Off.
    baseline = read_mstatus();
    uart_puts("boot: mstatus=");
    uart_put_hex(baseline);
    uart_puts(" FS=");
    uart_put_dec((unsigned long)fs_of(baseline));
    uart_puts("\n");
    check(fs_of(baseline) == 0, "boot mstatus FS field != 0 (Off)");

    // 2. Explicitly clear FS to Off, read back to prove the field
    // is really Off before the fault phase.
    __asm__ volatile("csrc mstatus, %0" :: "r"(FS_MASK));
    fs_off = read_mstatus();
    uart_puts("fs-off: mstatus=");
    uart_put_hex(fs_off);
    uart_puts(" FS=");
    uart_put_dec((unsigned long)fs_of(fs_off));
    uart_puts(" (expect 0)\n");
    check(fs_of(fs_off) == 0, "FS did not read back Off after csrc");

    // 3. Fault phase: fadd.d with FS=Off must trap. The expected
    // fault pc is taken from the instruction's own numeric label
    // (in-asm la, not a C labels-as-values address).
    __asm__ volatile(
        ".option push\n\t"
        ".option arch, +d\n\t"
        ".option norvc\n\t"
        "la %0, 1f\n\t"
        "1: fadd.d f0, f0, f0\n\t"  // faults here: FS=Off
        ".option pop\n\t"
        : "=r"(fault_site)
        :
        : "memory");

    uart_puts("fault: site=");
    uart_put_hex(fault_site);
    uart_puts(" traps=");
    uart_put_dec(sfo_trap[3]);
    uart_puts(" mcause=");
    uart_put_hex(sfo_trap[0]);
    uart_puts(" mepc=");
    uart_put_hex(sfo_trap[1]);
    uart_puts(" mtval=");
    uart_put_hex(sfo_trap[2]);
    uart_puts("\n");
    check(sfo_trap[3] == 1, "fault phase did not produce exactly one trap");
    check(sfo_trap[0] == MCAUSE_ILLEGAL_INST,
          "trap mcause != 0x2 (illegal instruction)");
    check(sfo_trap[1] == fault_site,
          "trap mepc != the fault site's recorded address");

    // 4. The trap must not have disturbed FS: it must still read
    // Off after the handler ran and returned.
    fs_after_trap = read_mstatus();
    uart_puts("after trap: FS=");
    uart_put_dec((unsigned long)fs_of(fs_after_trap));
    uart_puts(" (expect 0)\n");
    check(fs_of(fs_after_trap) == 0,
          "FS changed across the illegal-instruction trap");

    // 5. Positive control: FS=Initial, load f1=1.5 and f2=2.25,
    // then fadd.d f0,f1,f2 must execute with no new trap and f0
    // must read back exactly 3.75. The f0 bits move to an integer
    // register with fmv.x.d, so the readback is a pure bit check.
    __asm__ volatile("csrs mstatus, %0" :: "r"(1UL << 13));  // FS := Initial
    __asm__ volatile(
        ".option push\n\t"
        ".option arch, +d\n\t"
        "fmv.d.x f1, %1\n\t"        // f1 = 1.5
        "fmv.d.x f2, %2\n\t"        // f2 = 2.25
        "fadd.d f0, f1, f2\n\t"     // must execute, not trap
        "fmv.x.d %0, f0\n\t"        // f0 bits -> integer
        ".option pop\n\t"
        : "=r"(f0bits)
        : "r"(b1), "r"(b2)
        : "memory");
    fs_after_control = read_mstatus();
    uart_puts("control: traps=");
    uart_put_dec(sfo_trap[3]);
    uart_puts(" (expect still 1) f0bits=");
    uart_put_hex(f0bits);
    uart_puts(" (expect 0x400e000000000000) FS=");
    uart_put_dec((unsigned long)fs_of(fs_after_control));
    uart_puts(" (expect 3)\n");
    check(sfo_trap[3] == 1,
          "positive control trapped (FS=Initial fadd.d must execute)");
    check(f0bits == DBL_3_75,
          "fadd.d 1.5+2.25 did not produce 3.75 in f0");
    check(fs_of(fs_after_control) == (int)FS_DIRTY,
          "FP writes did not move FS to Dirty in the control");

    // 6. Restore the boot mstatus word exactly and confirm.
    __asm__ volatile("csrw mstatus, %0" :: "r"(baseline));
    restored = read_mstatus();
    uart_puts("restore: mstatus=");
    uart_put_hex(restored);
    uart_puts(" (expect boot word)\n");
    check(restored == baseline,
          "explicit csrw did not restore the boot mstatus word");

    // 7. No trap may have fired anywhere outside the one expected
    // fault.
    check(sfo_trap[3] == 1, "unexpected extra trap fired during the run");

    // Checksum over the measured words: boot mstatus, the FS-Off
    // readback, the recorded trap's mcause/mepc/mtval, the fault
    // site, the trap count, the control's f0 bits and FS, and the
    // restored mstatus. No absolute timing or host data enters.
    words[0] = baseline;
    words[1] = fs_off;
    words[2] = sfo_trap[0];
    words[3] = sfo_trap[1];
    words[4] = sfo_trap[2];
    words[5] = fault_site;
    words[6] = sfo_trap[3];
    words[7] = f0bits;
    words[8] = fs_after_control;
    words[9] = restored;
    checksum = fnv1a64((const unsigned char *)words, sizeof(words));

    uart_puts("checks=");
    uart_put_dec((unsigned long)checks);
    uart_puts(" mismatches=");
    uart_put_dec((unsigned long)fails);
    uart_puts("\n");
    uart_puts("checksum=");
    uart_put_hex(checksum);
    uart_puts("\n");
    uart_puts("RESULT: ");
    uart_puts(fails == 0 ? "PASS" : "FAIL");
    uart_puts("\n");

    // Let the UART drain before touching the finisher device.
    {
        volatile unsigned long i;
        for (i = 0; i < 200000UL; i++)
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
