// fvs_main.c: a static rounding mode encoded in the instruction
// overrides fcsr.frm (backlog item "riscv frm-dynamic-vs-static").
//
// Mechanism under test: in the F/D encoding the rm field of a
// floating-point arithmetic instruction selects the rounding mode
// for that instruction. rm=111 means "round with the dynamic mode
// in fcsr.frm"; any other rm value is a static override the hart
// must honor regardless of frm. The module runs in M-mode on QEMU
// 8.2.2 (virt) and executes fdiv.d 1.0/3.0 with the static RNE
// encoding (rm=000, assembled as `fdiv.d f10, f10, f11, rne`) under
// frm=RDN and under frm=RUP. Both quotients must be the RNE result
// 0x3FD5555555555555 and frm must read back unchanged after each
// divide. A dynamic control (the same fdiv.d with the rm operand
// omitted, which the assembler encodes as rm=111) runs under both
// frm values too: under RUP it must yield 0x3FD5555555555556,
// which differs from the static result and shows the divide really
// takes its rounding from the rm input; under RDN it yields
// 0x3FD5555555555555, the same bits the static encoding produces,
// which is the correct result for 1.0/3.0 rounded toward negative
// infinity (the true quotient sits above the RNE double, so
// rounding down lands on it).
//
// The operands travel as raw bit patterns in integer registers and
// are moved to f10/f11 with fmv.d.x inside volatile asm, so the
// compiler cannot constant-fold the divides; each division really
// executes on the hart. The FP instructions assemble under in-asm
// `.option arch, +d` because the module builds with
// -march=rv64imac_zicsr (no F/D); the compiler can never allocate
// FP registers under that march, so no register clobber is needed.
// objdump of the built ELF confirms the encodings: the static form
// assembles to 0x1ab50553 (rm field 000) and the dynamic form to
// 0x1ab57553 (rm field 111).
//
// On PASS the module writes the virt test-device finisher word
// 0x5555 at 0x100000, which shuts the machine down (QEMU exits 0).
// On FAIL it parks the hart in a wfi loop without touching the
// finisher; the bench harness runs QEMU under `timeout`, so a FAIL
// is observable as the timeout exit status (124) as well as the
// RESULT: FAIL line.

#include "../uart.h"

#define FCSR_FRM_SHIFT 5
#define FCSR_FRM_MASK (0x7UL << FCSR_FRM_SHIFT)
#define FCSR_FFLAGS_MASK 0x1fUL

#define MODE_RNE 0UL  // round to nearest, ties to even
#define MODE_RDN 2UL  // round toward negative infinity
#define MODE_RUP 3UL  // round toward positive infinity

#define NX_FLAG 0x01UL  // fflags bit 0: inexact

#define MISA_FD ((1UL << 5) | (1UL << 3))  // misa bits F (5) and D (3)
#define FS_MASK (0x3UL << 13)              // mstatus bits 14:13

#define CLINT_MTIME 0x0200bff8UL  // 64-bit mtime, UART-drain timebase

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// 1.0/3.0 as raw double bit patterns (opaque to the compiler: they
// arrive in integer registers, never as C doubles).
#define A_BITS 0x3FF0000000000000UL  // 1.0
#define B_BITS 0x4008000000000000UL  // 3.0

// Expected quotients, fixed by IEEE 754 for 1.0/3.0:
// RNE -> 0x3FD5555555555555; RDN -> 0x3FD5555555555555 (the RNE
// double lies below the true quotient, so rounding toward negative
// infinity lands on it); RUP -> 0x3FD5555555555556.
#define Q_RNE 0x3FD5555555555555UL
#define Q_RDN 0x3FD5555555555555UL
#define Q_RUP 0x3FD5555555555556UL

// Trap record: mcause, mepc, mtval at trap entry, trap counter.
// mscratch points here; boot.S clears BSS so the counter starts at 0.
static volatile unsigned long fvs_trap[4];

extern void fvs_trap_entry(void);

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

static void write_fcsr(unsigned long v) {
    __asm__ volatile("csrw fcsr, %0" :: "r"(v));
}

static unsigned long read_fcsr(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, fcsr" : "=r"(v));
    return v;
}

// fdiv.d with the STATIC RNE encoding (rm=000): the rounding mode
// comes from the instruction word, not from fcsr.frm.
static unsigned long fdiv_static_rne_bits(unsigned long a, unsigned long b) {
    unsigned long q;
    __asm__ volatile(
        ".option push\n\t"
        ".option arch, +d\n\t"
        "fmv.d.x f10, %1\n\t"
        "fmv.d.x f11, %2\n\t"
        "fdiv.d f10, f10, f11, rne\n\t"
        "fmv.x.d %0, f10\n\t"
        ".option pop"
        : "=r"(q)
        : "r"(a), "r"(b)
        : "memory");
    return q;
}

// fdiv.d with the DYNAMIC encoding (rm=111): the rounding mode comes
// from fcsr.frm at execution time.
static unsigned long fdiv_dynamic_bits(unsigned long a, unsigned long b) {
    unsigned long q;
    __asm__ volatile(
        ".option push\n\t"
        ".option arch, +d\n\t"
        "fmv.d.x f10, %1\n\t"
        "fmv.d.x f11, %2\n\t"
        "fdiv.d f10, f10, f11\n\t"
        "fmv.x.d %0, f10\n\t"
        ".option pop"
        : "=r"(q)
        : "r"(a), "r"(b)
        : "memory");
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

static void print_trial(const char *tag, unsigned long q, unsigned long fcsr) {
    uart_puts(tag);
    uart_puts(": q=");
    uart_put_hex(q);
    uart_puts(" fcsr=");
    uart_put_hex(fcsr);
    uart_puts(" (frm=");
    uart_put_dec((fcsr >> FCSR_FRM_SHIFT) & 0x7UL);
    uart_puts(" fflags=");
    uart_put_hex(fcsr & FCSR_FFLAGS_MASK);
    uart_puts(")\n");
}

int main(void) {
    unsigned long tv, mie, misa, mstatus_boot, fs_set;
    unsigned long q_s_rdn, fcsr_s_rdn, q_s_rup, fcsr_s_rup;
    unsigned long q_d_rdn, fcsr_d_rdn, q_d_rup, fcsr_d_rup;
    unsigned long words[12];
    unsigned long checksum;

    uart_init();
    uart_puts("frm-dynamic-vs-static: static rm in fdiv.d overrides fcsr.frm\n");

    // Safety net: counting M-mode trap handler. No trap is expected;
    // the run requires the counter to stay 0.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)fvs_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)fvs_trap));
    __asm__ volatile("csrci mstatus, 8");  // MIE clear: no M-mode interrupt
    __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    check((tv & ~3UL) == (unsigned long)fvs_trap_entry,
          "mtvec did not take the handler address");
    check((tv & 3UL) == 0, "mtvec not in direct mode");
    check(mie == 0, "mie nonzero at boot");

    // fdiv.d is a D-extension instruction; require misa to carry F
    // and D before using one.
    misa = read_misa();
    uart_puts("setup: misa=");
    uart_put_hex(misa);
    uart_puts("\n");
    check((misa & MISA_FD) == MISA_FD,
          "misa lacks the F/D extension bits (fdiv.d needs D)");

    // Boot baseline: FS must read 0 (Off); set it to Dirty before
    // any FP write, since an FP instruction with FS == Off raises
    // illegal-instruction.
    mstatus_boot = read_mstatus();
    check(((mstatus_boot >> 13) & 3UL) == 0,
          "boot mstatus FS field != 0 (Off)");
    __asm__ volatile("csrs mstatus, %0" :: "r"(3UL << 13));  // FS = Dirty
    fs_set = read_mstatus();
    uart_puts("setup: mstatus FS after set=");
    uart_put_dec((fs_set >> 13) & 3UL);
    uart_puts(" (expect 3=Dirty)\n");
    check(((fs_set >> 13) & 3UL) == 3,
          "mstatus FS did not read back Dirty after csrs");

    // Trial 1: static RNE under frm=RDN. The quotient must be the
    // RNE result and frm must read back RDN (untouched). The divide
    // is inexact, so fflags must show NX only.
    write_fcsr(MODE_RDN << FCSR_FRM_SHIFT);
    q_s_rdn = fdiv_static_rne_bits(A_BITS, B_BITS);
    fcsr_s_rdn = read_fcsr();
    print_trial("static-RNE under frm=RDN", q_s_rdn, fcsr_s_rdn);
    check(q_s_rdn == Q_RNE,
          "static-RNE quotient under frm=RDN != RNE result");
    check(((fcsr_s_rdn >> FCSR_FRM_SHIFT) & 0x7UL) == MODE_RDN,
          "frm changed by the static-RNE divide (was RDN)");
    check((fcsr_s_rdn & FCSR_FFLAGS_MASK) == NX_FLAG,
          "fflags after static-RNE divide under RDN != NX only");

    // Trial 2: static RNE under frm=RUP. Same expectations, with
    // frm=RUP surviving the divide.
    write_fcsr(MODE_RUP << FCSR_FRM_SHIFT);
    q_s_rup = fdiv_static_rne_bits(A_BITS, B_BITS);
    fcsr_s_rup = read_fcsr();
    print_trial("static-RNE under frm=RUP", q_s_rup, fcsr_s_rup);
    check(q_s_rup == Q_RNE,
          "static-RNE quotient under frm=RUP != RNE result");
    check(((fcsr_s_rup >> FCSR_FRM_SHIFT) & 0x7UL) == MODE_RUP,
          "frm changed by the static-RNE divide (was RUP)");
    check((fcsr_s_rup & FCSR_FFLAGS_MASK) == NX_FLAG,
          "fflags after static-RNE divide under RUP != NX only");

    // The two static quotients must agree with each other: the
    // override makes frm irrelevant.
    check(q_s_rdn == q_s_rup,
          "static-RNE quotients under RDN and RUP disagree");

    // Control 1: dynamic fdiv.d under frm=RDN. Must follow frm and
    // yield the RDN quotient.
    write_fcsr(MODE_RDN << FCSR_FRM_SHIFT);
    q_d_rdn = fdiv_dynamic_bits(A_BITS, B_BITS);
    fcsr_d_rdn = read_fcsr();
    print_trial("dynamic under frm=RDN", q_d_rdn, fcsr_d_rdn);
    check(q_d_rdn == Q_RDN,
          "dynamic quotient under frm=RDN != RDN result");
    check((fcsr_d_rdn & FCSR_FFLAGS_MASK) == NX_FLAG,
          "fflags after dynamic divide under RDN != NX only");

    // Control 2: dynamic fdiv.d under frm=RUP. Must follow frm and
    // yield the RUP quotient, which differs from the static RNE
    // result: this divergence is what proves the divide really
    // rounds per its rm input, so the static agreement above is a
    // genuine override and not a dead rounding path.
    write_fcsr(MODE_RUP << FCSR_FRM_SHIFT);
    q_d_rup = fdiv_dynamic_bits(A_BITS, B_BITS);
    fcsr_d_rup = read_fcsr();
    print_trial("dynamic under frm=RUP", q_d_rup, fcsr_d_rup);
    check(q_d_rup == Q_RUP,
          "dynamic quotient under frm=RUP != RUP result");
    check(q_d_rup != q_s_rup,
          "dynamic quotient under RUP equals the static-RNE result "
          "(rounding path looks dead)");
    check((fcsr_d_rup & FCSR_FFLAGS_MASK) == NX_FLAG,
          "fflags after dynamic divide under RUP != NX only");

    // No trap may have fired during the run.
    uart_puts("traps: count=");
    uart_put_dec(fvs_trap[3]);
    uart_puts(" mcause=");
    uart_put_hex(fvs_trap[0]);
    uart_puts(" mepc=");
    uart_put_hex(fvs_trap[1]);
    uart_puts(" mtval=");
    uart_put_hex(fvs_trap[2]);
    uart_puts("\n");
    check(fvs_trap[3] == 0, "trap handler fired during the run");

    // Checksum over the logged measurement words, in print order.
    words[0] = misa;
    words[1] = mstatus_boot;
    words[2] = fs_set;
    words[3] = q_s_rdn;
    words[4] = fcsr_s_rdn;
    words[5] = q_s_rup;
    words[6] = fcsr_s_rup;
    words[7] = q_d_rdn;
    words[8] = fcsr_d_rdn;
    words[9] = q_d_rup;
    words[10] = fcsr_d_rup;
    words[11] = fvs_trap[3];
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
