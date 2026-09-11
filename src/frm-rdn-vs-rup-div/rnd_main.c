// rnd_main.c: directed rounding on fdiv.d 1.0/3.0 (backlog item
// "riscv frm-rdn-vs-rup-div").
//
// Mechanism under test: the dynamic rounding mode in fcsr.frm
// selects which of the two doubles bracketing the true quotient is
// returned by fdiv.d. With frm=RDN (round toward negative
// infinity) the divide must return the lower bracket; with frm=RUP
// (round toward positive infinity) it must return the upper
// bracket. For 1.0/3.0 the two brackets are exactly 1 ulp apart,
// so the module checks, on the quotient bit patterns as unsigned
// 64-bit integers: q_rup == q_rdn + 1, (q_rup ^ q_rdn) == 1, and
// q_rup > q_rdn. An RNE trial runs as a logged anchor showing
// where round-to-nearest falls relative to the pair; the verdict
// rests on the RDN/RUP checks only.
//
// The operands travel as raw bit patterns in integer registers and
// are moved to f10/f11 with fmv.d.x inside volatile asm, so the
// compiler cannot constant-fold the divides; each division really
// executes on the hart. The FP instructions assemble under in-asm
// `.option arch, +d` because the module builds with
// -march=rv64imac_zicsr (no F/D); the compiler can never allocate
// FP registers under that march, so no register clobber is needed.
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

// fcsr write values: frm in bits 5:7, fflags clear.
#define FCSR_RDN_WRITE 0x40UL  // frm=RDN (2), fflags=0
#define FCSR_RUP_WRITE 0x60UL  // frm=RUP (3), fflags=0
#define FCSR_RNE_WRITE 0x00UL  // frm=RNE (0), fflags=0 (anchor trial)

// Trap record: mcause, mepc, mtval at trap entry, trap counter.
// mscratch points here; boot.S clears BSS so the counter starts at 0.
static volatile unsigned long rnd_trap[4];

extern void rnd_trap_entry(void);

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

// fdiv.d with the DYNAMIC encoding (rm field omitted, encoded as
// 111): the rounding mode comes from fcsr.frm at execution time.
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
    unsigned long rb_rdn, q_rdn, fcsr_rdn;
    unsigned long rb_rup, q_rup, fcsr_rup;
    unsigned long q_rne, fcsr_rne;
    unsigned long words[10];
    unsigned long checksum;

    uart_init();
    uart_puts("frm-rdn-vs-rup-div: RDN vs RUP rounding on fdiv.d 1.0/3.0\n");

    // Safety net: counting M-mode trap handler. No trap is expected;
    // the run requires the counter to stay 0.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)rnd_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)rnd_trap));
    __asm__ volatile("csrci mstatus, 8");  // MIE clear: no M-mode interrupt
    __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    check((tv & ~3UL) == (unsigned long)rnd_trap_entry,
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

    // Trial 1: frm=RDN. Write fcsr=0x40 (frm=2, flags clear) and
    // require the readback to be exactly 0x40; divide; require the
    // frm field to survive the divide and fflags to show NX only
    // (1.0/3.0 is inexact, nothing else).
    write_fcsr(FCSR_RDN_WRITE);
    rb_rdn = read_fcsr();
    uart_puts("frm-RDN: write fcsr=0x40 readback=");
    uart_put_hex(rb_rdn);
    uart_puts("\n");
    check(rb_rdn == FCSR_RDN_WRITE,
          "fcsr did not read back 0x40 after the RDN write");
    q_rdn = fdiv_dynamic_bits(A_BITS, B_BITS);
    fcsr_rdn = read_fcsr();
    print_trial("frm-RDN", q_rdn, fcsr_rdn);
    check(((fcsr_rdn >> FCSR_FRM_SHIFT) & 0x7UL) == MODE_RDN,
          "frm changed by the divide under RDN");
    check((fcsr_rdn & FCSR_FFLAGS_MASK) == NX_FLAG,
          "fflags after divide under RDN != NX only");

    // Trial 2: frm=RUP. Write fcsr=0x60 (frm=3, flags clear) and
    // require the readback to be exactly 0x60; divide; require frm
    // to survive and fflags to show NX only.
    write_fcsr(FCSR_RUP_WRITE);
    rb_rup = read_fcsr();
    uart_puts("frm-RUP: write fcsr=0x60 readback=");
    uart_put_hex(rb_rup);
    uart_puts("\n");
    check(rb_rup == FCSR_RUP_WRITE,
          "fcsr did not read back 0x60 after the RUP write");
    q_rup = fdiv_dynamic_bits(A_BITS, B_BITS);
    fcsr_rup = read_fcsr();
    print_trial("frm-RUP", q_rup, fcsr_rup);
    check(((fcsr_rup >> FCSR_FRM_SHIFT) & 0x7UL) == MODE_RUP,
          "frm changed by the divide under RUP");
    check((fcsr_rup & FCSR_FFLAGS_MASK) == NX_FLAG,
          "fflags after divide under RUP != NX only");

    // Anchor: the same divide under frm=RNE, logged only. The
    // verdict rests on the RDN/RUP checks; this line shows where
    // round-to-nearest falls relative to the pair.
    write_fcsr(FCSR_RNE_WRITE);
    q_rne = fdiv_dynamic_bits(A_BITS, B_BITS);
    fcsr_rne = read_fcsr();
    uart_puts("anchor-RNE: q=");
    uart_put_hex(q_rne);
    uart_puts(" fcsr=");
    uart_put_hex(fcsr_rne);
    uart_puts(" (frm=");
    uart_put_dec((fcsr_rne >> FCSR_FRM_SHIFT) & 0x7UL);
    uart_puts(" fflags=");
    uart_put_hex(fcsr_rne & FCSR_FFLAGS_MASK);
    uart_puts(") equals-q_rdn=");
    uart_put_dec(q_rne == q_rdn ? 1 : 0);
    uart_puts(" one-below-q_rup=");
    uart_put_dec(q_rne + 1UL == q_rup ? 1 : 0);
    uart_puts("\n");

    // The verdict checks, on the quotient bit patterns as unsigned
    // 64-bit integers: RDN and RUP must pick adjacent doubles with
    // the RUP result the higher neighbor. (Correction to the backlog
    // item's stated xor expectation: it said q_rup ^ q_rdn == 1, but
    // 0x3FD5555555555555 ^ 0x3FD5555555555556 = 0x3, not 0x1,
    // because 0x5 ^ 0x6 = 0x3: the +1 flips the low two bits of the
    // ...0101 tail. The true claim is that the patterns differ in
    // exactly the two low bits, which the == 0x3 check asserts; the
    // == q_rdn + 1 and > checks already carry the "exactly 1 ulp
    // apart, RUP the higher neighbor" claim.)
    uart_puts("ulp: q_rdn=");
    uart_put_hex(q_rdn);
    uart_puts(" q_rup=");
    uart_put_hex(q_rup);
    uart_puts(" (rup-rdn=");
    uart_put_hex(q_rup - q_rdn);
    uart_puts(" xor=");
    uart_put_hex(q_rup ^ q_rdn);
    uart_puts(")\n");
    check(q_rup == q_rdn + 1UL,
          "q_rup != q_rdn + 1 (quotients not 1 ulp apart)");
    check((q_rup ^ q_rdn) == 0x3UL,
          "q_rup ^ q_rdn != 0x3 (expect 0x3FD5555555555555 ^ "
          "0x3FD5555555555556 = 0x3: low two bits differ)");
    check(q_rup > q_rdn,
          "q_rup <= q_rdn (RUP must pick the higher neighbor)");

    // No trap may have fired during the run.
    uart_puts("traps: count=");
    uart_put_dec(rnd_trap[3]);
    uart_puts(" mcause=");
    uart_put_hex(rnd_trap[0]);
    uart_puts(" mepc=");
    uart_put_hex(rnd_trap[1]);
    uart_puts(" mtval=");
    uart_put_hex(rnd_trap[2]);
    uart_puts("\n");
    check(rnd_trap[3] == 0, "trap handler fired during the run");

    // Checksum over the logged measurement words, in print order.
    words[0] = misa;
    words[1] = mstatus_boot;
    words[2] = fs_set;
    words[3] = q_rdn;
    words[4] = fcsr_rdn;
    words[5] = q_rup;
    words[6] = fcsr_rup;
    words[7] = q_rne;
    words[8] = fcsr_rne;
    words[9] = rnd_trap[3];
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
