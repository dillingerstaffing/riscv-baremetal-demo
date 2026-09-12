// fnxo_main.c: fflags.NX is the accrued inexact flag (backlog
// item "riscv fflags-nx-only").
//
// Mechanism under test: the fflags field of the fcsr CSR is the
// accrued-exception record of floating-point operations, and
// NX (bit 0) accrues exactly when an operation's result is not
// exactly representable. One inexact floating-point operation
// must set NX and move no other accrued flag; an exact
// operation must move none. The module runs in M-mode on QEMU
// 8.2.2 (virt machine) and walks this sequence:
//
//   1. boot baseline: read mstatus, require FS == 0 (Off); set
//      FS to Initial (1) with csrs, because an FP instruction
//      with FS == Off would raise illegal-instruction. Clear
//      mstatus.MIE and assert mie == 0 at boot so no interrupt
//      can fire; misa must carry the F and D extension bits
//      because the sequence executes fadd.d.
//   2. Phase A (inexact): csrw fcsr, 0x00 (frm=RNE, fflags=0)
//      and require the readback to be exactly 0x00. Execute
//      one volatile in-asm fadd.d with rm=DYN on the operand
//      pair (1e16, 1.0). The exact sum 10^16 + 1 is not
//      representable (derivation below), so NX must set and
//      NV/DZ/OF/UF must stay clear: (fcsr & 0x1f) == 0x01, and
//      the result bits must equal the correctly rounded value.
//      frm must still read 0.
//   3. Phase B (exact control): csrw fcsr, 0x00 again, exact
//      readback again. Execute fadd.d on (1.0, 2.0); the exact
//      sum 3.0 is representable, so (fcsr & 0x1f) must read
//      0x00: no accrued flag moved.
//   4. restore fcsr to its exact boot value and require the
//      readback to match; FS sanity: after all FP writes the
//      mstatus FS field must not read Off.
//   5. the trap counter must be 0.
//
// The operands are loaded as bit patterns moved into FP
// registers with fmv.d.x from integer registers, and results
// are read back with fmv.x.d, so no host floating point is
// involved in feeding the operations or reading their results.
//
// A counting M-mode trap handler (fnxo_trap.S) is installed as
// a safety net; the run requires its counter to stay 0. Each
// phase's result/fcsr pair is printed, and a 64-bit FNV-1a
// checksum over the logged measurement words is printed for the
// PROOF.md results table. RESULT: PASS prints only when every
// check holds.
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
// blocks touch no memory. GAS assembles `, dyn` as rm=111, the
// dynamic rounding mode, so the instruction rounds with the frm
// field the phase just wrote (RNE, from fcsr=0x00).

#include "../uart.h"

#define FS_MASK    (0x3UL << 13)   // mstatus bits 14:13
#define FS_INITIAL 1UL
#define SD_BIT     (1UL << 63)    // mstatus bit 63, summary of FS/XS/VS
#define MISA_FD    ((1UL << 5) | (1UL << 3))  // misa bits F (5) and D (3)

#define FFLAGS_NX  0x01UL         // fcsr bit 0: inexact
#define FFLAGS_ALL 0x1FUL         // fcsr bits 4:0: NV|DZ|OF|UF|NX
#define FRM_MASK   (0x7UL << 5)   // fcsr bits 7:5
#define FRM_RNE    0UL            // round to nearest, ties to even

// Phase A operands: 1e16 and 1.0 as double bit patterns.
#define E16_BITS 0x4341C37937E08000UL  // 1e16, exactly representable
#define ONE_BITS 0x3FF0000000000000UL  // 1.0
// Correctly rounded fadd.d(1e16, 1.0) under RNE. Derivation
// (checked in PROOF.md, no host FP involved at run time):
// 10^16 = 2^16 * 5^16 with 5^16 = 152587890625, so 10^16 is
// exactly a double: exponent field 0x434 (e=53), 53-bit
// significand 152587890625 * 2^15 = 5000000000000000000, LSB 0
// (even). In the binade [2^53, 2^54) the ulp is 2, so the two
// representable neighbors of the exact sum 10^16 + 1 are
// 10^16 (significand even) and 10^16 + 2 (significand odd).
// The exact sum sits exactly halfway between them; RNE
// ties-to-even picks the even significand, i.e. 10^16. The
// exact sum is not representable, so NX must set.
#define ADD_INEXACT_BITS 0x4341C37937E08000UL

// Phase B operands: 1.0 + 2.0 = 3.0, exactly representable,
// so no accrued flag may move.
#define TWO_BITS   0x4000000000000000UL  // 2.0
#define THREE_BITS 0x4008000000000000UL  // 3.0, the exact sum

#define CLINT_MTIME 0x0200bff8UL  // 64-bit mtime, UART-drain timebase

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// Trap record: mcause, mepc, mtval at trap entry, trap counter.
// mscratch points here; boot.S clears BSS so the counter starts at 0.
static volatile unsigned long fnxo_trap[4];

extern void fnxo_trap_entry(void);

static unsigned long read_fcsr(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, fcsr" : "=r"(v));
    return v;
}

static void write_fcsr(unsigned long v) {
    __asm__ volatile("csrw fcsr, %0" :: "r"(v));
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

// One volatile fadd.d with rm=DYN, so the instruction rounds with
// the frm field the phase set beforehand. Operands move through
// integer registers only; the result bits come back through one.
static unsigned long fadd_dyn_bits(unsigned long a, unsigned long b) {
    unsigned long r;
    __asm__ volatile(
        ".option push\n\t"
        ".option arch, +d\n\t"
        "fmv.d.x f2, %1\n\t"
        "fmv.d.x f3, %2\n\t"
        "fadd.d f1, f2, f3, dyn\n\t"   // rounds with the frm field
        "fmv.x.d %0, f1\n\t"
        ".option pop\n\t"
        : "=r"(r)
        : "r"(a), "r"(b));
    return r;
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
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// Run one phase: write fcsr (sets frm and clears fflags in one
// write), require the exact readback, execute the addition with
// dynamic rounding, read back result bits and fcsr. Prints the
// measured pair; the verdict comes from the code-computed
// checks.
static void phase(const char *name, unsigned long a, unsigned long b,
                  unsigned long expect_bits, unsigned long expect_fflags,
                  unsigned long *words, int *wi) {
    unsigned long r, f, wrote = 0x00;

    write_fcsr(wrote);
    f = read_fcsr();

    uart_puts(name);
    uart_puts(": fcsr-write=");
    uart_put_hex(wrote);
    uart_puts(" fcsr-readback=");
    uart_put_hex(f);

    // The write must read back exactly: frm=RNE took, fflags
    // cleared, no other bit moved.
    checks++;
    check(f == wrote, "fcsr readback != 0x00 after the clear write");

    r = fadd_dyn_bits(a, b);
    f = read_fcsr();

    uart_puts(" result=");
    uart_put_hex(r);
    uart_puts(" fcsr=");
    uart_put_hex(f);
    uart_puts(" fflags=");
    uart_put_hex(f & FFLAGS_ALL);
    uart_puts(" frm=");
    uart_put_dec((f >> 5) & 7UL);
    uart_puts("\n");

    // The result bits must equal the analytically derived
    // expectation (inexact rounded value, or the exact sum).
    checks++;
    if (r != expect_bits) {
        uart_puts("  FAIL: result bits != expected for ");
        uart_puts(name);
        uart_puts("\n");
        fails++;
    }
    // Accrued-flag isolation: the inexact phase must move NX
    // alone; the exact phase must move nothing.
    checks++;
    if ((f & FFLAGS_ALL) != expect_fflags) {
        uart_puts("  FAIL: fflags != expected for ");
        uart_puts(name);
        uart_puts("\n");
        fails++;
    }
    // The frm field must still read the mode this phase set.
    checks++;
    if (((f >> 5) & 7UL) != FRM_RNE) {
        uart_puts("  FAIL: frm field moved during ");
        uart_puts(name);
        uart_puts("\n");
        fails++;
    }

    words[(*wi)++] = r;
    words[(*wi)++] = f;
}

int main(void) {
    unsigned long tv, mie, misa;
    unsigned long baseline, after_initial, mstatus_end;
    unsigned long boot_fcsr, fcsr_restored;
    unsigned long words[16];
    int wi = 0;
    unsigned long checksum;

    uart_init();
    uart_puts("fflags-nx-only: one inexact fadd.d sets NX alone, an exact fadd.d sets nothing\n");

    // Safety net: counting M-mode trap handler. No trap is expected;
    // the run requires the counter to stay 0.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)fnxo_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)fnxo_trap));
    __asm__ volatile("csrci mstatus, 8");  // MIE clear: no M-mode interrupt
    __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    uart_puts("setup: mtvec=");
    uart_put_hex(tv);
    uart_puts(" mie=");
    uart_put_hex(mie);
    uart_puts("\n");
    checks++; check((tv & ~3UL) == (unsigned long)fnxo_trap_entry,
                    "mtvec did not take the handler address");
    checks++; check((tv & 3UL) == 0, "mtvec not in direct mode");
    checks++; check(mie == 0, "mie nonzero at boot");

    // fadd.d is a D-extension instruction; require misa to carry
    // F and D before executing one.
    misa = read_misa();
    uart_puts("setup: misa=");
    uart_put_hex(misa);
    uart_puts("\n");
    checks++; check((misa & MISA_FD) == MISA_FD,
                    "misa lacks the F/D extension bits (fadd.d needs D)");

    // Boot baseline: FS must read 0 (Off).
    baseline = read_mstatus();
    checks++; check(fs_of(baseline) == 0,
                    "boot mstatus FS field != 0 (Off)");

    // FS := Initial (1). Required before any FP use: with FS ==
    // Off an FP instruction would raise illegal-instruction.
    __asm__ volatile("csrs mstatus, %0" :: "r"(1UL << 13));
    after_initial = read_mstatus();
    checks++; check(fs_of(after_initial) == (int)FS_INITIAL,
                    "FS did not read back Initial (1) after csrs");
    checks++; check(other_bits_same(after_initial, baseline) &&
                    sd_of(after_initial) == 0,
                    "a non-FS bit changed, or SD set, when FS=Initial");

    // Boot fcsr value, recorded so the run can restore it exactly
    // at the end.
    boot_fcsr = read_fcsr();
    uart_puts("setup: fcsr(boot)=");
    uart_put_hex(boot_fcsr);
    uart_puts("\n");

    // Phase A (inexact): 1e16 + 1.0. The exact sum needs 54
    // significant bits and is not representable, so NX must set
    // and NV/DZ/OF/UF must stay clear. The correctly rounded
    // (RNE ties-to-even) result is 1e16 itself.
    phase("A", E16_BITS, ONE_BITS, ADD_INEXACT_BITS, FFLAGS_NX, words, &wi);

    // Phase B (exact control): 1.0 + 2.0 = 3.0. The exact sum is
    // representable, so no accrued flag may move.
    phase("B", ONE_BITS, TWO_BITS, THREE_BITS, 0x00, words, &wi);

    // Restore fcsr to its exact boot value and require the
    // readback to match.
    write_fcsr(boot_fcsr);
    fcsr_restored = read_fcsr();
    uart_puts("end: fcsr(restored)=");
    uart_put_hex(fcsr_restored);
    uart_puts("\n");
    checks++; check(fcsr_restored == boot_fcsr,
                    "fcsr did not restore to its boot value");

    // FS sanity: the FP writes above must have left FS out of
    // Off (the FP state was genuinely touched).
    mstatus_end = read_mstatus();
    uart_puts("end: mstatus=");
    uart_put_hex(mstatus_end);
    uart_puts(" FS=");
    uart_put_dec((unsigned long)fs_of(mstatus_end));
    uart_puts("\n");
    checks++; check(fs_of(mstatus_end) != 0,
                    "FS still Off after the FP operations");

    // No trap may have fired during the run.
    uart_puts("traps: count=");
    uart_put_dec(fnxo_trap[3]);
    uart_puts(" mcause=");
    uart_put_hex(fnxo_trap[0]);
    uart_puts(" mepc=");
    uart_put_hex(fnxo_trap[1]);
    uart_puts(" mtval=");
    uart_put_hex(fnxo_trap[2]);
    uart_puts("\n");
    checks++; check(fnxo_trap[3] == 0, "trap handler fired during the run");

    // Checksum over the logged measurement words: the two
    // result/fcsr pairs, then misa, boot fcsr, and the two
    // mstatus readbacks.
    words[wi++] = misa;
    words[wi++] = boot_fcsr;
    words[wi++] = baseline;
    words[wi++] = mstatus_end;
    words[wi++] = fnxo_trap[3];
    checksum = fnv1a64((const unsigned char *)words,
                       (unsigned long)wi * sizeof(unsigned long));

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
