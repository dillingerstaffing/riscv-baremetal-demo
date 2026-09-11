// ffu_main.c: fflags UF (underflow) bit accrual on an underflowing fmul.d
// (backlog item "riscv fflags-uf-underflow").
//
// Mechanism under test: the fflags field of the fcsr CSR is the
// accrued-exception record of floating-point operations. The UF
// bit (fflags bit 4) must be set by an operation whose exact
// result is tiny and inexact, i.e. an underflow per IEEE 754.
// The module multiplies DBL_MIN by itself: the exact product is
// 2^-2044, far below the smallest normal double 2^-1022, so it is
// tiny, and the rounded result (0) differs from the exact
// nonzero product, so it is inexact. Tiny plus inexact is the
// underflow condition, so UF must set; the delivered result is
// inexact, so NX must set too. No other flag bit may move.
//
// Operand choice justification (IEEE 754, from the binary format
// itself, no host floating point involved): DBL_MIN is the
// smallest positive normal double, bit pattern
// 0x0010000000000000 (exponent field 1, emin = -1022).
// DBL_MIN * DBL_MIN = 2^-2044. The exact exponent -2044 is below
// emin, so the exact result cannot be represented as a normal;
// the rounded result is 0 (2^-2044 is far below even the
// smallest subnormal 2^-1074), which differs from the exact
// nonzero 2^-2044, so the operation is inexact. A tiny, inexact
// result is exactly the underflow case, hence UF. (A nearby
// alternative, DBL_MIN * 0.5, was rejected: its exact result
// 2^-1023 is exactly representable as a subnormal, hence exact,
// hence NOT an underflow; testing it would prove nothing about
// UF.)
//
// The module runs in M-mode on QEMU 8.2.2 (virt machine) and
// walks this sequence:
//
//   1. read the boot mstatus; require FS == 0 (Off).
//   2. set FS to Initial (1) with csrs. Executing an FP
//      instruction with FS == Off raises an illegal-instruction
//      exception, so this step must come before any FP use; the
//      module would trap without it.
//   3. clear fflags with csrw fcsr, x0 (frm = RNE); require the
//      fcsr readback to be exactly 0x00.
//   4. execute the underflowing double multiply DBL_MIN * DBL_MIN
//      as a real in-asm volatile fmul.d; require the fcsr
//      readback to be exactly 0x3, i.e. UF (bit 1) and NX
//      (bit 0) set, NV/DZ/OF all 0,
//      all 0, frm still RNE.
//   5. clear fcsr again; require the readback to be exactly 0x00.
//   6. sanity anchor: the product read back with fmv.x.d must be
//      0x0000000000000000 (the underflowed result rounds to
//      zero). The anchor is logged; the verdict rests on the
//      fflags checks.
//
// The operands are loaded as bit patterns (0x0010000000000000)
// moved into FP registers with fmv.d.x from integer registers,
// so no host floating point is involved in feeding the
// operation.
//
// A counting M-mode trap handler (ffu_trap.S) is installed as a
// safety net; the run requires its counter to stay 0.
// mstatus.MIE is cleared and mie is asserted 0 at boot so no
// interrupt can fire. misa is read and asserted to carry the F
// and D extension bits, because fmul.d is a D-extension
// instruction.
//
// The fcsr read triple (before the multiply, after it, cleared)
// is printed, every check is computed in code, and a 64-bit
// FNV-1a checksum over the logged measurement words is printed
// for the PROOF.md results table. RESULT: PASS prints only when
// every check holds.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop
// without touching the finisher; the bench harness runs QEMU
// under `timeout`, so a FAIL is observable as the timeout exit
// status (124) in addition to the RESULT: FAIL line.
//
// The FP instructions assemble under in-asm `.option arch, +d`
// because the module builds with -march=rv64imac_zicsr (no F/D).
// The compiler can never allocate f1/f2/f3 (its march has no
// floating point), so no register clobber is needed; the asm
// blocks touch no memory. The fmul.d operand words and product
// move through integer registers only.

#include "../uart.h"

#define FS_MASK    (0x3UL << 13)   // mstatus bits 14:13
#define FS_INITIAL 1UL
#define SD_BIT     (1UL << 63)    // mstatus bit 63, summary of FS/XS/VS
#define MISA_FD    ((1UL << 5) | (1UL << 3))  // misa bits F (5) and D (3)

#define FFLAGS_NX  0x01UL         // fcsr bit 0: inexact
#define FFLAGS_UF  0x02UL         // fcsr bit 1: underflow
#define FFLAGS_ALL 0x1FUL         // fcsr bits 4:0: NV|DZ|OF|UF|NX
#define FRM_MASK   (0x7UL << 5)   // fcsr bits 7:5
#define FRM_RNE    0UL            // round to nearest, ties to even

#define DBLMIN_BITS 0x0010000000000000UL  // DBL_MIN, the smallest normal double
#define PROD_BITS   0x0000000000000000UL  // expected product: underflowed to zero

#define CLINT_MTIME 0x0200bff8UL  // 64-bit mtime, UART-drain timebase

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// Trap record: mcause, mepc, mtval at trap entry, trap counter.
// mscratch points here; boot.S clears BSS so the counter starts at 0.
static volatile unsigned long ffu_trap[4];

extern void ffu_trap_entry(void);

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

// One volatile underflowing multiply, then the product's bit
// pattern back through an integer register. The volatile asm
// keeps the compiler from constant-folding the operation or
// dropping it.
static unsigned long mul_dblmin_sq_bits(void) {
    unsigned long p;
    __asm__ volatile(
        ".option push\n\t"
        ".option arch, +d\n\t"
        "fmv.d.x f2, %1\n\t"
        "fmv.d.x f3, %1\n\t"
        "fmul.d f1, f2, f3, rne\n\t"   // the real instruction under test
        "fmv.x.d %0, f1\n\t"
        ".option pop\n\t"
        : "=r"(p)
        : "r"(DBLMIN_BITS));
    return p;
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
    unsigned long p;
    unsigned long words[8];
    unsigned long checksum;
    int checks = 0;

    uart_init();
    uart_puts("fflags-uf-underflow: underflowing fmul.d must set fflags.UF+NX\n");

    // Safety net: counting M-mode trap handler. No trap is expected;
    // the run requires the counter to stay 0.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)ffu_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)ffu_trap));
    __asm__ volatile("csrci mstatus, 8");  // MIE clear: no M-mode interrupt
    __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    uart_puts("setup: mtvec=");
    uart_put_hex(tv);
    uart_puts(" mie=");
    uart_put_hex(mie);
    uart_puts("\n");
    checks++; check((tv & ~3UL) == (unsigned long)ffu_trap_entry,
                    "mtvec did not take the handler address");
    checks++; check((tv & 3UL) == 0, "mtvec not in direct mode");
    checks++; check(mie == 0, "mie nonzero at boot");

    // fmul.d is a D-extension instruction; require misa to carry F
    // and D before executing one.
    misa = read_misa();
    uart_puts("setup: misa=");
    uart_put_hex(misa);
    uart_puts("\n");
    checks++; check((misa & MISA_FD) == MISA_FD,
                    "misa lacks the F/D extension bits (fmul.d needs D)");

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

    // 4. One underflowing multiply, DBL_MIN * DBL_MIN: fcsr must
    // read back exactly 0x3 (UF=bit 1 and NX=bit 0 set, NV/DZ/OF
    // all 0, frm still RNE).
    p = mul_dblmin_sq_bits();
    fcsr_after = read_fcsr();
    print_fcsr("after ", fcsr_after);
    uart_puts("product bits=");
    uart_put_hex(p);
    uart_puts("\n");
    checks++; check((fcsr_after & FFLAGS_ALL) == (FFLAGS_UF | FFLAGS_NX),
                    "fflags != 0x3 (UF+NX) after the underflowing fmul.d");
    checks++; check((fcsr_after & FFLAGS_ALL) == (FFLAGS_UF | FFLAGS_NX) &&
                    (fcsr_after & FFLAGS_UF) != 0 &&
                    (fcsr_after & FFLAGS_NX) != 0,
                    "UF or NX individually missing after the underflow");
    checks++; check((fcsr_after & ~FFLAGS_ALL) == 0UL,
                    "a non-fflags bit (reserved or frm) moved after fmul.d");
    checks++; check(((fcsr_after >> 5) & 7UL) == FRM_RNE,
                    "frm != RNE after the underflowing fmul.d");

    // 5. Sanity anchor: the product is the underflowed-to-zero
    // double. Logged for the record; the verdict rests on the
    // fflags checks.
    checks++; check(p == PROD_BITS,
                    "product bits != 0x0000000000000000");

    // 6. Clear fflags again; require the readback to be exactly 0x00.
    clear_fcsr();
    fcsr_cleared = read_fcsr();
    print_fcsr("cleared", fcsr_cleared);
    checks++; check(fcsr_cleared == 0x00UL,
                    "fcsr readback != 0x00 after the second clear");

    // No trap may have fired during the run.
    uart_puts("traps: count=");
    uart_put_dec(ffu_trap[3]);
    uart_puts(" mcause=");
    uart_put_hex(ffu_trap[0]);
    uart_puts(" mepc=");
    uart_put_hex(ffu_trap[1]);
    uart_puts(" mtval=");
    uart_put_hex(ffu_trap[2]);
    uart_puts("\n");
    checks++; check(ffu_trap[3] == 0, "trap handler fired during the run");

    // fcsr read triple: before the multiply, after it, cleared.
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
    words[3] = p;
    words[4] = baseline;
    words[5] = after_initial;
    words[6] = misa;
    words[7] = ffu_trap[3];
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
