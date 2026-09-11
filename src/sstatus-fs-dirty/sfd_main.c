// sfd_main.c: mstatus.FS Clean->Dirty transition on an FP register
// write (backlog item "riscv sstatus-fs-dirty").
//
// Mechanism under test: the FS field (bits 14:13) of mstatus is the
// record of floating-point state dirtiness. The module runs in
// M-mode on QEMU 8.2.2 (virt machine) and walks this sequence:
//
//   1. read the boot mstatus; require FS == 0 (Off).
//   2. set FS to Initial (1) with csrs. Executing an FP instruction
//      with FS == Off raises an illegal-instruction exception, so
//      this step must come before any FP write; the module would
//      trap without it.
//   3. execute one FP register write (fmv.d.x f1, x0); require the
//      FS field to read back as Dirty (3).
//   4. write a second FP register (fmv.d.x f2, x0); require FS to
//      stay 3 (sticky Dirty).
//   5. clear FS to 0 with csrc; require the full mstatus word to
//      read back identical to the boot baseline, then restore the
//      boot mstatus word exactly with csrw.
//
// A counting M-mode trap handler (sfd_trap.S) is installed as a
// safety net; the run requires its counter to stay 0. mstatus.MIE
// is cleared and mie is asserted 0 at boot so no interrupt can
// fire. misa is read and asserted to carry the F and D extension
// bits, because fmv.d.x is a D-extension instruction.
//
// The read triple (FS after step 2, after step 3, after step 5) is
// printed, every check is computed in code, and a 64-bit FNV-1a
// checksum over the logged measurement words is printed for the
// PROOF.md results table. RESULT: PASS prints only when every
// check holds.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop
// without touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.
//
// The FP writes use in-asm `.option arch, +d` because the module
// builds with -march=rv64imac_zicsr (no F/D). The compiler can never
// allocate f1/f2 (its march has no floating point), so no register
// clobber is needed; the asm touches no memory and has no outputs.

#include "../uart.h"

#define FS_MASK    (0x3UL << 13)   // mstatus bits 14:13
#define FS_INITIAL 1UL
#define FS_DIRTY   3UL
#define SD_BIT     (1UL << 63)    // mstatus bit 63, summary of FS/XS/VS
#define MISA_FD    ((1UL << 5) | (1UL << 3))  // misa bits F (5) and D (3)

#define CLINT_MTIME 0x0200bff8UL  // 64-bit mtime, UART-drain timebase

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// Trap record: mcause, mepc, mtval at trap entry, trap counter.
// mscratch points here; boot.S clears BSS so the counter starts at 0.
static volatile unsigned long sfd_trap[4];

extern void sfd_trap_entry(void);

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

static unsigned long read_mtime(void) {
    return *(volatile unsigned long *)CLINT_MTIME;
}

static void fp_write_f1(void) {
    __asm__ volatile(
        ".option push\n\t"
        ".option arch, +d\n\t"
        "fmv.d.x f1, x0\n\t"   // one FP register write; must set FS to Dirty
        ".option pop\n\t");
}

static void fp_write_f2(void) {
    __asm__ volatile(
        ".option push\n\t"
        ".option arch, +d\n\t"
        "fmv.d.x f2, x0\n\t"   // second FP register write; FS must stay Dirty
        ".option pop\n\t");
}

static int fs_of(unsigned long v) {
    return (int)((v >> 13) & 3UL);
}

static int sd_of(unsigned long v) {
    return (int)((v >> 63) & 1UL);
}

// True when a and b agree on every mstatus bit except the FS field
// and the SD summary bit.
static int other_bits_same(unsigned long a, unsigned long b) {
    return (a & ~(FS_MASK | SD_BIT)) == (b & ~(FS_MASK | SD_BIT));
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

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static void print_fs_state(const char *tag, unsigned long v) {
    uart_puts(tag);
    uart_puts(": mstatus=");
    uart_put_hex(v);
    uart_puts(" FS=");
    uart_put_dec((unsigned long)fs_of(v));
    uart_puts(" SD=");
    uart_put_dec((unsigned long)sd_of(v));
    uart_puts("\n");
}

int main(void) {
    unsigned long tv, mie, misa;
    unsigned long baseline, after_initial, after_f1, after_f2;
    unsigned long after_clear, restored;
    unsigned long words[8];
    unsigned long checksum;
    int checks = 0;

    uart_init();
    uart_puts("sstatus-fs-dirty: FP register write moves mstatus.FS Off->Dirty\n");

    // Safety net: counting M-mode trap handler. No trap is expected;
    // the run requires the counter to stay 0.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)sfd_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)sfd_trap));
    __asm__ volatile("csrci mstatus, 8");  // MIE clear: no M-mode interrupt
    __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    uart_puts("setup: mtvec=");
    uart_put_hex(tv);
    uart_puts(" mie=");
    uart_put_hex(mie);
    uart_puts("\n");
    checks++; check((tv & ~3UL) == (unsigned long)sfd_trap_entry,
                    "mtvec did not take the handler address");
    checks++; check((tv & 3UL) == 0, "mtvec not in direct mode");
    checks++; check(mie == 0, "mie nonzero at boot");

    // fmv.d.x is a D-extension instruction; require misa to carry F
    // and D before executing one.
    misa = read_misa();
    uart_puts("setup: misa=");
    uart_put_hex(misa);
    uart_puts("\n");
    checks++; check((misa & MISA_FD) == MISA_FD,
                    "misa lacks the F/D extension bits (fmv.d.x needs D)");

    // 1. Boot baseline: FS must read 0 (Off).
    baseline = read_mstatus();
    print_fs_state("boot    ", baseline);
    checks++; check(fs_of(baseline) == 0,
                    "boot mstatus FS field != 0 (Off)");

    // 2. FS := Initial (1). Required before any FP write: with FS ==
    // Off the FP instruction below would raise illegal-instruction.
    __asm__ volatile("csrs mstatus, %0" :: "r"(1UL << 13));
    after_initial = read_mstatus();
    print_fs_state("initial ", after_initial);
    checks++; check(fs_of(after_initial) == (int)FS_INITIAL,
                    "FS did not read back Initial (1) after csrs");
    checks++; check(other_bits_same(after_initial, baseline) &&
                    sd_of(after_initial) == 0,
                    "a non-FS bit changed, or SD set, when FS=Initial");

    // 3. One FP register write: FS must move to Dirty (3).
    fp_write_f1();
    after_f1 = read_mstatus();
    print_fs_state("after f1", after_f1);
    checks++; check(fs_of(after_f1) == (int)FS_DIRTY,
                    "one fmv.d.x did not move FS to Dirty (3)");
    checks++; check(other_bits_same(after_f1, baseline) &&
                    sd_of(after_f1) == 1,
                    "a non-FS non-SD bit changed, or SD clear, after fmv.d.x");

    // 4. Second FP register write: FS must stay Dirty (sticky).
    fp_write_f2();
    after_f2 = read_mstatus();
    print_fs_state("after f2", after_f2);
    checks++; check(fs_of(after_f2) == (int)FS_DIRTY,
                    "FS did not stay Dirty (3) after the second fmv.d.x");
    checks++; check(other_bits_same(after_f2, baseline) &&
                    sd_of(after_f2) == 1,
                    "a non-FS non-SD bit changed on the second fmv.d.x");

    // 5. Clear FS to 0: the full mstatus word must read back
    // identical to the boot baseline, then restore the baseline
    // word exactly with csrw and confirm.
    __asm__ volatile("csrc mstatus, %0" :: "r"(FS_MASK));
    after_clear = read_mstatus();
    print_fs_state("cleared ", after_clear);
    checks++; check(after_clear == baseline,
                    "clearing FS did not restore the exact boot mstatus word");
    __asm__ volatile("csrw mstatus, %0" :: "r"(baseline));
    restored = read_mstatus();
    checks++; check(restored == baseline,
                    "explicit csrw did not restore the boot mstatus word");

    // No trap may have fired during the run.
    uart_puts("traps: count=");
    uart_put_dec(sfd_trap[3]);
    uart_puts(" mcause=");
    uart_put_hex(sfd_trap[0]);
    uart_puts(" mepc=");
    uart_put_hex(sfd_trap[1]);
    uart_puts(" mtval=");
    uart_put_hex(sfd_trap[2]);
    uart_puts("\n");
    checks++; check(sfd_trap[3] == 0, "trap handler fired during the run");

    // Read triple: FS before the FP write, after it, and cleared.
    uart_puts("triple FS: before=");
    uart_put_dec((unsigned long)fs_of(after_initial));
    uart_puts(" after=");
    uart_put_dec((unsigned long)fs_of(after_f1));
    uart_puts(" cleared=");
    uart_put_dec((unsigned long)fs_of(after_clear));
    uart_puts("\n");

    // Checksum over the logged measurement words.
    words[0] = baseline;
    words[1] = after_initial;
    words[2] = after_f1;
    words[3] = after_f2;
    words[4] = after_clear;
    words[5] = restored;
    words[6] = misa;
    words[7] = sfd_trap[3];
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
