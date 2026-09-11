// frw_main.c: fcsr.frm is a writable field; every rounding mode
// reads back (backlog item "riscv frm-rounding-write").
//
// Mechanism under test: fcsr holds the accrued exception flags in
// bits 4:0 (fflags) and the dynamic rounding mode in bits 7:5
// (frm). The module runs in M-mode on QEMU 8.2.2 (virt machine)
// and walks this sequence:
//
//   1. install the counting trap handler safety net, clear
//      mstatus.MIE, assert mie == 0; read misa and require the F
//      and D extension bits (the sanity anchor executes fdiv.d).
//   2. read the boot mstatus; require FS == 0 (Off); set FS to
//      Initial (1) with csrs. An FP instruction with FS == Off
//      would raise illegal-instruction, so this comes before any
//      FP register write; the FP div in the sanity anchor is such
//      a write.
//   3. for each mode in {RNE=0, RTZ=1, RDN=2, RUP=3, RMM=4}: write
//      fcsr = (mode << 5) with csrw (fflags written 0), read fcsr
//      back, require the frm field to equal the written mode and
//      the fflags field to still be 0 (the write must not touch
//      the flags).
//   4. sanity anchor (logged, verdict rests on step 3): under RDN
//      then RUP, divide 1.0 by 3.0 with fdiv.d (operands loaded by
//      fmv.d.x from integer registers holding the exact double
//      bit patterns 0x3FF0000000000000 and 0x4008000000000000, in
//      volatile asm so nothing is constant-folded) and log the
//      two quotient bit patterns against the expected pair
//      0x3FD5555555555555 / 0x3FD5555555555556, proving the written
//      mode steers hardware rounding.
//   5. restore frm=RNE (csrw fcsr, 0) and require the full fcsr
//      word to read back 0x00.
//
// The written-vs-readback frm pairs are printed every run. A
// 64-bit FNV-1a checksum over the logged measurement words is
// printed; RESULT: PASS prints only when every readback check
// holds. On PASS the module writes the virt test-device finisher
// word 0x5555 at 0x100000, which shuts the machine down and QEMU
// exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.
//
// The FP instructions assemble under in-asm `.option arch, +d`
// because the module builds with -march=rv64imac_zicsr (no F/D);
// the compiler can never allocate FP registers under that march,
// so no register clobber is needed.
//
// This module covers only the frm field. mstatus.FS is covered by
// sstatus-fs-dirty; the fflags field is a sibling item.

#include "../uart.h"

#define FCSR_FRM_SHIFT 5
#define FCSR_FFLAGS_MASK 0x1fUL
#define FCSR_FRM_MASK (0x7UL << FCSR_FRM_SHIFT)

#define MODE_RNE 0UL  // round to nearest, ties to even
#define MODE_RTZ 1UL  // round toward zero
#define MODE_RDN 2UL  // round toward negative infinity
#define MODE_RUP 3UL  // round toward positive infinity
#define MODE_RMM 4UL  // round to nearest, ties to max magnitude

#define MISA_FD ((1UL << 5) | (1UL << 3))  // misa bits F (5) and D (3)
#define FS_MASK (0x3UL << 13)  // mstatus bits 14:13

#define CLINT_MTIME 0x0200bff8UL  // 64-bit mtime, UART-drain timebase

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// Trap record: mcause, mepc, mtval at trap entry, trap counter.
// mscratch points here; boot.S clears BSS so the counter starts at 0.
static volatile unsigned long frw_trap[4];

extern void frw_trap_entry(void);

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

static unsigned long frm_of(unsigned long fcsr) {
    return (fcsr >> FCSR_FRM_SHIFT) & 0x7UL;
}

static unsigned long fflags_of(unsigned long fcsr) {
    return fcsr & FCSR_FFLAGS_MASK;
}

// Divide two doubles given as raw bit patterns and return the
// quotient as a raw bit pattern. The operands arrive in integer
// registers via fmv.d.x, so the compiler cannot constant-fold the
// division; the mode in fcsr.frm at call time steers the rounding.
static unsigned long fdiv_bits(unsigned long a, unsigned long b) {
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

int main(void) {
    unsigned long tv, mie, misa, mstatus_boot;
    unsigned long rdbk[5], final_fcsr;
    unsigned long q_rdn, q_rup;
    unsigned long words[11];
    unsigned long checksum;
    int m;
    static const unsigned long modes[5] =
        { MODE_RNE, MODE_RTZ, MODE_RDN, MODE_RUP, MODE_RMM };
    static const char *mode_names[5] =
        { "RNE", "RTZ", "RDN", "RUP", "RMM" };

    uart_init();
    uart_puts("frm-rounding-write: every fcsr.frm mode writes and reads back\n");

    // Safety net: counting M-mode trap handler. No trap is expected;
    // the run requires the counter to stay 0.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)frw_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)frw_trap));
    __asm__ volatile("csrci mstatus, 8");  // MIE clear: no M-mode interrupt
    __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    check((tv & ~3UL) == (unsigned long)frw_trap_entry,
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

    // Boot baseline: FS must read 0 (Off); set it to Initial before
    // any FP write, since an FP instruction with FS == Off raises
    // illegal-instruction.
    mstatus_boot = read_mstatus();
    check(((mstatus_boot >> 13) & 3UL) == 0,
          "boot mstatus FS field != 0 (Off)");
    __asm__ volatile("csrs mstatus, %0" :: "r"(1UL << 13));

    // 3. Write each rounding mode, read back, require the frm field
    // to match and fflags to be untouched (still 0).
    write_fcsr(0);
    for (m = 0; m < 5; m++) {
        unsigned long mode = modes[m];
        unsigned long written = mode << FCSR_FRM_SHIFT;
        write_fcsr(written);
        rdbk[m] = read_fcsr();
        uart_puts("mode ");
        uart_puts(mode_names[m]);
        uart_puts(": written frm=");
        uart_put_dec(mode);
        uart_puts(" readback fcsr=");
        uart_put_hex(rdbk[m]);
        uart_puts(" frm=");
        uart_put_dec(frm_of(rdbk[m]));
        uart_puts(" fflags=");
        uart_put_dec(fflags_of(rdbk[m]));
        uart_puts("\n");
        check(frm_of(rdbk[m]) == mode,
              "frm did not read back the written mode");
        check(fflags_of(rdbk[m]) == 0,
              "fflags were not 0 after the fcsr write");
    }

    // 4. Sanity anchor, logged only (verdict rests on the readback
    // checks above): the written mode must steer hardware rounding
    // on a real divide. fdiv.d 1.0/3.0 is inexact, so fflags gain
    // NX here; the fcsr is restored in step 5.
    write_fcsr(MODE_RDN << FCSR_FRM_SHIFT);
    q_rdn = fdiv_bits(0x3FF0000000000000UL, 0x4008000000000000UL);
    write_fcsr(MODE_RUP << FCSR_FRM_SHIFT);
    q_rup = fdiv_bits(0x3FF0000000000000UL, 0x4008000000000000UL);
    uart_puts("anchor RDN q=");
    uart_put_hex(q_rdn);
    uart_puts(" expected=0x3fd5555555555555 ok=");
    uart_put_dec((unsigned long)(q_rdn == 0x3FD5555555555555UL));
    uart_puts("\n");
    uart_puts("anchor RUP q=");
    uart_put_hex(q_rup);
    uart_puts(" expected=0x3fd5555555555556 ok=");
    uart_put_dec((unsigned long)(q_rup == 0x3FD5555555555556UL));
    uart_puts("\n");

    // 5. Restore frm=RNE and require the full fcsr word to read
    // back 0x00 (frm cleared, NX from the anchor cleared).
    write_fcsr(0);
    final_fcsr = read_fcsr();
    uart_puts("restored fcsr=");
    uart_put_hex(final_fcsr);
    uart_puts("\n");
    check(final_fcsr == 0, "restored fcsr did not read back 0x00");

    // No trap may have fired during the run.
    uart_puts("traps: count=");
    uart_put_dec(frw_trap[3]);
    uart_puts(" mcause=");
    uart_put_hex(frw_trap[0]);
    uart_puts(" mepc=");
    uart_put_hex(frw_trap[1]);
    uart_puts(" mtval=");
    uart_put_hex(frw_trap[2]);
    uart_puts("\n");
    check(frw_trap[3] == 0, "trap handler fired during the run");

    // Checksum over the logged measurement words.
    words[0] = mstatus_boot;
    words[1] = misa;
    words[2] = rdbk[0];
    words[3] = rdbk[1];
    words[4] = rdbk[2];
    words[5] = rdbk[3];
    words[6] = rdbk[4];
    words[7] = q_rdn;
    words[8] = q_rup;
    words[9] = final_fcsr;
    words[10] = frw_trap[3];
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
