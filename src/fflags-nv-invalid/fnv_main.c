// fnv_main.c: fflags NV (invalid operation) bit accrual on
// fdiv.d 0.0/0.0 (backlog item "riscv fflags-nv-invalid").
//
// Mechanism under test: the fflags field of the fcsr CSR is the
// accrued-exception record of floating-point operations. The NV
// bit (fflags bit 4) must be set by an invalid operation such as
// 0.0/0.0, with no other accrued bit moving. The module runs in
// M-mode on QEMU 8.2.2 (virt machine) and walks this sequence:
//
//   1. read the boot mstatus; require FS == 0 (Off).
//   2. set FS to Initial (1) with csrs. Executing an FP instruction
//      with FS == Off raises an illegal-instruction exception, so
//      this step must come before any FP use; the module would
//      trap without it.
//   3. clear fflags with csrw fcsr, x0 (frm = RNE); require the
//      fcsr readback to be exactly 0x00.
//   4. execute one invalid divide, 0.0/0.0, as a real in-asm
//      fdiv.d (volatile asm so the compiler cannot constant-fold
//      or eliminate it); require the fcsr readback to be exactly
//      0x10, i.e. NV set, NX/UF/OF/DZ all clear, and frm still RNE.
//   5. clear fcsr again; require the readback to be exactly 0x00.
//   6. sanity anchor: the quotient read back with fmv.x.d must
//      equal the canonical quiet NaN, 0x7FF8000000000000. The
//      anchor is logged; the verdict rests on the fflags checks.
//
// The operands are loaded as bit patterns (0x0000000000000000 for
// both zeros) moved into FP registers with fmv.d.x from integer
// registers, so no host floating point is involved in feeding the
// operation.
//
// A counting M-mode trap handler (fnv_trap.S) is installed as a
// safety net; the run requires its counter to stay 0. mstatus.MIE
// is cleared and mie is asserted 0 at boot so no interrupt can
// fire. misa is read and asserted to carry the F and D extension
// bits, because fdiv.d is a D-extension instruction.
//
// The fcsr read triple (before the divide, after it, cleared) is
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
// The FP instructions assemble under in-asm `.option arch, +d`
// because the module builds with -march=rv64imac_zicsr (no F/D).
// The compiler can never allocate f1/f2/f3 (its march has no
// floating point), so no register clobber is needed; the asm
// blocks touch no memory. The fdiv.d operand words and quotient
// move through integer registers only.

#include "../uart.h"

#define FS_MASK    (0x3UL << 13)   // mstatus bits 14:13
#define FS_INITIAL 1UL
#define SD_BIT     (1UL << 63)    // mstatus bit 63, summary of FS/XS/VS
#define MISA_FD    ((1UL << 5) | (1UL << 3))  // misa bits F (5) and D (3)

#define FFLAGS_NV  0x10UL         // fcsr bit 4: invalid operation
#define FFLAGS_ALL 0x1FUL         // fcsr bits 4:0: NV|DZ|OF|UF|NX
#define FRM_MASK   (0x7UL << 5)   // fcsr bits 7:5
#define FRM_RNE    0UL            // round to nearest, ties to even

#define ZERO_BITS  0x0000000000000000UL  // +0.0 as a double bit pattern
#define QNAN_BITS  0x7FF8000000000000UL  // canonical quiet NaN bit pattern

#define CLINT_MTIME 0x0200bff8UL  // 64-bit mtime, UART-drain timebase

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// Trap record: mcause, mepc, mtval at trap entry, trap counter.
// mscratch points here; boot.S clears BSS so the counter starts at 0.
static volatile unsigned long fnv_trap[4];

extern void fnv_trap_entry(void);

static unsigned long read_fcsr(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, fcsr" : "=r"(v));
    return v;
}

static void clear_fcsr(void) {
    __asm__ volatile("csrw fcsr, x0");  // fflags=0, frm=RNE
}

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

// One volatile invalid divide, 0.0/0.0, then the quotient's bit
// pattern back through an integer register. The volatile asm keeps
// the compiler from constant-folding the operation or dropping it.
static unsigned long div00_bits(void) {
    unsigned long q;
    __asm__ volatile(
        ".option push\n\t"
        ".option arch, +d\n\t"
        "fmv.d.x f2, %1\n\t"
        "fmv.d.x f3, %2\n\t"
        "fdiv.d f1, f2, f3, rne\n\t"   // the real instruction under test
        "fmv.x.d %0, f1\n\t"
        ".option pop\n\t"
        : "=r"(q)
        : "r"(ZERO_BITS), "r"(ZERO_BITS));
    return q;
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

static void print_fcsr(const char *tag, unsigned long v) {
    uart_puts(tag);
    uart_puts(": fcsr=");
    uart_put_hex(v);
    uart_puts(" fflags=");
    uart_put_hex(v & FFLAGS_ALL);
    uart_puts(" frm=");
    uart_put_dec((v >> 5) & 7UL);
    uart_puts("\n");
}

int main(void) {
    unsigned long tv, mie, misa;
    unsigned long baseline, after_initial;
    unsigned long fcsr_before, fcsr_after, fcsr_cleared;
    unsigned long q;
    unsigned long words[8];
    unsigned long checksum;
    int checks = 0;

    uart_init();
    uart_puts("fflags-nv-invalid: fdiv.d 0.0/0.0 must set fflags.NV\n");

    // Safety net: counting M-mode trap handler. No trap is expected;
    // the run requires the counter to stay 0.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)fnv_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)fnv_trap));
    __asm__ volatile("csrci mstatus, 8");  // MIE clear: no M-mode interrupt
    __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    uart_puts("setup: mtvec=");
    uart_put_hex(tv);
    uart_puts(" mie=");
    uart_put_hex(mie);
    uart_puts("\n");
    checks++; check((tv & ~3UL) == (unsigned long)fnv_trap_entry,
                    "mtvec did not take the handler address");
    checks++; check((tv & 3UL) == 0, "mtvec not in direct mode");
    checks++; check(mie == 0, "mie nonzero at boot");

    // fdiv.d is a D-extension instruction; require misa to carry F
    // and D before executing one.
    misa = read_misa();
    uart_puts("setup: misa=");
    uart_put_hex(misa);
    uart_puts("\n");
    checks++; check((misa & MISA_FD) == MISA_FD,
                    "misa lacks the F/D extension bits (fdiv.d needs D)");

    // 1. Boot baseline: FS must read 0 (Off).
    baseline = read_mstatus();
    checks++; check(fs_of(baseline) == 0,
                    "boot mstatus FS field != 0 (Off)");

    // 2. FS := Initial (1). Required before any FP use: with FS ==
    // Off an FP instruction would raise illegal-instruction.
    __asm__ volatile("csrs mstatus, %0" :: "r"(1UL << 13));
    after_initial = read_mstatus();
    checks++; check(fs_of(after_initial) == (int)FS_INITIAL,
                    "FS did not read back Initial (1) after csrs");
    checks++; check(other_bits_same(after_initial, baseline) &&
                    sd_of(after_initial) == 0,
                    "a non-FS bit changed, or SD set, when FS=Initial");

    // 3. Clear fflags with csrw fcsr, x0 (frm=RNE); require the
    // readback to be exactly 0x00.
    clear_fcsr();
    fcsr_before = read_fcsr();
    print_fcsr("before", fcsr_before);
    checks++; check(fcsr_before == 0x00UL,
                    "fcsr readback != 0x00 after clear");

    // 4. One invalid divide, 0.0/0.0: fcsr must read back exactly
    // 0x10 (NV set, NX/UF/OF/DZ all clear, frm still RNE).
    q = div00_bits();
    fcsr_after = read_fcsr();
    print_fcsr("after ", fcsr_after);
    uart_puts("quotient bits=");
    uart_put_hex(q);
    uart_puts("\n");
    checks++; check((fcsr_after & FFLAGS_ALL) == FFLAGS_NV,
                    "fflags != 0x10 (NV) after fdiv.d 0.0/0.0");
    checks++; check((fcsr_after & ~FFLAGS_ALL) == 0UL,
                    "a non-fflags bit (reserved or frm) moved after fdiv.d");
    checks++; check(((fcsr_after >> 5) & 7UL) == FRM_RNE,
                    "frm != RNE after fdiv.d 0.0/0.0");

    // 5. Sanity anchor: the quotient is the canonical quiet NaN.
    // Logged for the record; the verdict rests on the fflags checks.
    checks++; check(q == QNAN_BITS,
                    "quotient bits != 0x7FF8000000000000 (canonical quiet NaN)");

    // 6. Clear fflags again; require the readback to be exactly 0x00.
    clear_fcsr();
    fcsr_cleared = read_fcsr();
    print_fcsr("cleared", fcsr_cleared);
    checks++; check(fcsr_cleared == 0x00UL,
                    "fcsr readback != 0x00 after the second clear");

    // No trap may have fired during the run.
    uart_puts("traps: count=");
    uart_put_dec(fnv_trap[3]);
    uart_puts(" mcause=");
    uart_put_hex(fnv_trap[0]);
    uart_puts(" mepc=");
    uart_put_hex(fnv_trap[1]);
    uart_puts(" mtval=");
    uart_put_hex(fnv_trap[2]);
    uart_puts("\n");
    checks++; check(fnv_trap[3] == 0, "trap handler fired during the run");

    // fcsr read triple: before the divide, after it, cleared.
    uart_puts("triple fcsr: before=");
    uart_put_hex(fcsr_before);
    uart_puts(" after=");
    uart_put_hex(fcsr_after);
    uart_puts(" cleared=");
    uart_put_hex(fcsr_cleared);
    uart_puts("\n");

    // Checksum over the logged measurement words.
    words[0] = fcsr_before;
    words[1] = fcsr_after;
    words[2] = fcsr_cleared;
    words[3] = q;
    words[4] = baseline;
    words[5] = after_initial;
    words[6] = misa;
    words[7] = fnv_trap[3];
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
