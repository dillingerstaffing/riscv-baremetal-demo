// frd_main.c: the fcsr frm field selects the rounding direction
// of inexact FP results (backlog item "riscv fcsr-frm-rounddn").
//
// Mechanism under test: the frm field of the fcsr CSR (bits 7:5)
// is the dynamic rounding mode. An FP instruction whose rm field
// is DYN (111) must round its infinitely-precise result with the
// rounding function frm names, so the same inexact subtraction
// must deliver the lower neighbor under RDN and the upper
// neighbor under RNE, and the operation must set only the NX
// flag in fflags. The module runs in M-mode on QEMU 8.2.2 (virt
// machine) and walks this sequence:
//
//   1. boot baseline: read mstatus, require FS == 0 (Off); set
//      FS to Initial (1) with csrs, because an FP instruction
//      with FS == Off would raise illegal-instruction. Clear
//      mstatus.MIE and assert mie == 0 at boot so no interrupt
//      can fire; misa must carry the F and D extension bits
//      because the sequence executes fsub.d.
//   2. trial RDN: csrw fcsr, 0x40 (frm=RDN, fflags=0); require
//      the readback to be exactly 0x40. Execute one volatile
//      in-asm fsub.d with rm=DYN on the operand pair
//      (1 + 3*2^-52) - 2^-55, whose exact difference is
//      1 + 2.875*2^-52, strictly between the adjacent doubles
//      0x3FF0000000000002 and 0x3FF0000000000003 and closer to
//      the upper one (7/8 ulp below, 1/8 ulp above; proven in
//      PROOF.md from the binary expansions, not from the
//      machine). Rounding toward -inf must deliver the lower
//      neighbor 0x3FF0000000000002. Assert the result bits,
//      fflags == 0x01 (NX set, no other flag), and the frm field
//      still reading 2.
//   3. trial RNE: csrw fcsr, 0x00; require the readback to be
//      exactly 0x00. Same subtraction; the nearer candidate is
//      the upper one, so expect 0x3FF0000000000003. Same three
//      assertions, plus the neighbor relation between the two
//      trials' results: q_rdn == q_rne - 1 as unsigned 64-bit
//      values, and q_rdn < q_rne.
//   4. restore fcsr to its boot value (0x00) and require the
//      readback to match exactly.
//   5. FS sanity: after all FP writes the mstatus FS field must
//      not read Off (the FP state was genuinely touched).
//   6. the trap counter must be 0.
//
// The operands are loaded as bit patterns (0x3FF0000000000003
// and 0x3C80000000000000) moved into FP registers with fmv.d.x
// from integer registers, and results are read back with
// fmv.x.d, so no host floating point is involved in feeding the
// operations or reading their results.
//
// A counting M-mode trap handler (frd_trap.S) is installed as a
// safety net; the run requires its counter to stay 0. Each
// trial's result/fcsr pair is printed, and a 64-bit FNV-1a
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
// dynamic rounding mode, which is exactly what makes the
// instruction consult the frm field the trial just wrote.

#include "../uart.h"

#define FS_MASK    (0x3UL << 13)   // mstatus bits 14:13
#define FS_INITIAL 1UL
#define SD_BIT     (1UL << 63)    // mstatus bit 63, summary of FS/XS/VS
#define MISA_FD    ((1UL << 5) | (1UL << 3))  // misa bits F (5) and D (3)

#define FFLAGS_NX  0x01UL         // fcsr bit 0: inexact
#define FFLAGS_ALL 0x1FUL         // fcsr bits 4:0: NV|DZ|OF|UF|NX
#define FRM_MASK   (0x7UL << 5)   // fcsr bits 7:5
#define FRM_RNE    0UL            // round to nearest, ties to even
#define FRM_RDN    2UL            // round toward -inf

#define A_BITS 0x3FF0000000000003UL  // 1 + 3*2^-52 as a double bit pattern
#define B_BITS 0x3C80000000000000UL  // 2^-55 as a double bit pattern

#define SUB_RDN_BITS 0x3FF0000000000002UL  // RDN of the inexact difference
#define SUB_RNE_BITS 0x3FF0000000000003UL  // RNE of the inexact difference

#define CLINT_MTIME 0x0200bff8UL  // 64-bit mtime, UART-drain timebase

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// Trap record: mcause, mepc, mtval at trap entry, trap counter.
// mscratch points here; boot.S clears BSS so the counter starts at 0.
static volatile unsigned long frd_trap[4];

extern void frd_trap_entry(void);

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

// One volatile fsub.d with rm=DYN, so the instruction rounds with
// the frm field the trial set beforehand. Operands move through
// integer registers only; the result bits come back through one.
static unsigned long fsub_dyn_bits(unsigned long a, unsigned long b) {
    unsigned long r;
    __asm__ volatile(
        ".option push\n\t"
        ".option arch, +d\n\t"
        "fmv.d.x f2, %1\n\t"
        "fmv.d.x f3, %2\n\t"
        "fsub.d f1, f2, f3, dyn\n\t"   // rounds with the frm field
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

static const char *frm_name(unsigned long frm) {
    switch (frm) {
    case FRM_RNE: return "RNE";
    case FRM_RDN: return "RDN";
    default:      return "???";
    }
}

// Run one trial: write fcsr (sets the frm field and clears fflags
// in one write), require the exact readback, execute the
// subtraction with dynamic rounding, read back result bits and
// fcsr. Prints the measured pair; the verdict comes from the
// code-computed checks.
static unsigned long trial(const char *vec, unsigned long fcsr_write,
                           unsigned long frm, unsigned long a,
                           unsigned long b, unsigned long expect_bits,
                           unsigned long *words, int *wi) {
    unsigned long r, f;

    write_fcsr(fcsr_write);
    f = read_fcsr();

    uart_puts(vec);
    uart_puts(" ");
    uart_puts(frm_name(frm));
    uart_puts(": fcsr-write=");
    uart_put_hex(fcsr_write);
    uart_puts(" fcsr-readback=");
    uart_put_hex(f);

    // The write must read back exactly: frm field took the mode,
    // fflags cleared, no other bit moved.
    checks++;
    if (f != fcsr_write) {
        uart_puts("  FAIL: fcsr readback != written value for ");
        uart_puts(vec);
        uart_puts(" ");
        uart_puts(frm_name(frm));
        uart_puts("\n");
        fails++;
    }

    r = fsub_dyn_bits(a, b);
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
    // expectation: frm selected the rounding direction.
    checks++;
    if (r != expect_bits) {
        uart_puts("  FAIL: result bits != expected for ");
        uart_puts(vec);
        uart_puts(" ");
        uart_puts(frm_name(frm));
        uart_puts("\n");
        fails++;
    }
    // NX set, no other flag: the operation was inexact and
    // nothing else.
    checks++;
    if ((f & FFLAGS_ALL) != FFLAGS_NX) {
        uart_puts("  FAIL: fflags != NX for ");
        uart_puts(vec);
        uart_puts(" ");
        uart_puts(frm_name(frm));
        uart_puts("\n");
        fails++;
    }
    // The frm field must still read the mode this trial set.
    checks++;
    if (((f >> 5) & 7UL) != frm) {
        uart_puts("  FAIL: frm field moved during ");
        uart_puts(vec);
        uart_puts(" ");
        uart_puts(frm_name(frm));
        uart_puts("\n");
        fails++;
    }

    words[(*wi)++] = r;
    words[(*wi)++] = f;
    return r;
}

int main(void) {
    unsigned long tv, mie, misa;
    unsigned long baseline, after_initial, mstatus_end;
    unsigned long boot_fcsr, fcsr_restored;
    unsigned long q_rdn, q_rne;
    unsigned long words[16];
    int wi = 0;
    unsigned long checksum;

    uart_init();
    uart_puts("fcsr-frm-rounddn: frm=RDN rounds an inexact fsub.d toward -inf\n");

    // Safety net: counting M-mode trap handler. No trap is expected;
    // the run requires the counter to stay 0.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)frd_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)frd_trap));
    __asm__ volatile("csrci mstatus, 8");  // MIE clear: no M-mode interrupt
    __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    uart_puts("setup: mtvec=");
    uart_put_hex(tv);
    uart_puts(" mie=");
    uart_put_hex(mie);
    uart_puts("\n");
    checks++; check((tv & ~3UL) == (unsigned long)frd_trap_entry,
                    "mtvec did not take the handler address");
    checks++; check((tv & 3UL) == 0, "mtvec not in direct mode");
    checks++; check(mie == 0, "mie nonzero at boot");

    // fsub.d is a D-extension instruction; require misa to carry
    // F and D before executing one.
    misa = read_misa();
    uart_puts("setup: misa=");
    uart_put_hex(misa);
    uart_puts("\n");
    checks++; check((misa & MISA_FD) == MISA_FD,
                    "misa lacks the F/D extension bits (fsub.d needs D)");

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

    // Trial RDN: frm=RDN (2), fflags=0 in one write; the exact
    // difference 1 + 2.875*2^-52 is strictly between
    // 0x3FF0000000000002 and 0x3FF0000000000003, so rounding
    // toward -inf must deliver the lower neighbor.
    q_rdn = trial("sub", FRM_RDN << 5, FRM_RDN, A_BITS, B_BITS,
                  SUB_RDN_BITS, words, &wi);

    // Trial RNE: frm=RNE (0), fflags=0 in one write; the exact
    // difference is closer to the upper candidate (1/8 ulp vs
    // 7/8 ulp), so the nearer candidate 0x3FF0000000000003 must
    // win.
    q_rne = trial("sub", FRM_RNE << 5, FRM_RNE, A_BITS, B_BITS,
                  SUB_RNE_BITS, words, &wi);

    // Neighbor relation between the two trials: the RDN result
    // must be exactly the lower neighbor of the RNE result.
    uart_puts("rel: q_rdn=");
    uart_put_hex(q_rdn);
    uart_puts(" q_rne=");
    uart_put_hex(q_rne);
    uart_puts("\n");
    checks++; check(q_rdn == q_rne - 1UL,
                    "q_rdn != q_rne - 1 as unsigned 64-bit values");
    checks++; check(q_rdn < q_rne, "q_rdn not < q_rne");

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
    uart_put_dec(frd_trap[3]);
    uart_puts(" mcause=");
    uart_put_hex(frd_trap[0]);
    uart_puts(" mepc=");
    uart_put_hex(frd_trap[1]);
    uart_puts(" mtval=");
    uart_put_hex(frd_trap[2]);
    uart_puts("\n");
    checks++; check(frd_trap[3] == 0, "trap handler fired during the run");

    // Checksum over the logged measurement words: the two
    // result/fcsr pairs, then misa, boot fcsr, and the two
    // mstatus readbacks.
    words[wi++] = misa;
    words[wi++] = boot_fcsr;
    words[wi++] = baseline;
    words[wi++] = mstatus_end;
    words[wi++] = frd_trap[3];
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
