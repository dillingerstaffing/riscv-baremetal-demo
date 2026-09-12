// ffi_main.c: fcsr fflags/frm field independence (backlog item
// "riscv fcsr-field-independence").
//
// Mechanism under test: the fcsr CSR holds two independent
// fields, fflags (bits 4:0, the accrued exception flags) and
// frm (bits 7:5, the dynamic rounding mode). A CSR
// read-modify-write on one field, via the csrs/csrc
// instructions, must not disturb the other. The module runs in
// M-mode on QEMU 8.2.2 (virt machine) and walks this sequence:
//
//   1. boot baseline: read mstatus, require FS == 0 (Off); set
//      FS to Initial (1) with csrs, because an FP instruction
//      with FS == Off would raise illegal-instruction. Clear
//      mstatus.MIE and assert mie == 0 at boot so no interrupt
//      can fire; misa must carry the F and D extension bits
//      because the anchor sequence executes fdiv.d.
//   2. csrw fcsr, x0 (frm=RNE, fflags clear); require the fcsr
//      readback to be exactly 0x00.
//   3. execute one inexact fdiv.d, 1.0/3.0, as a real volatile
//      in-asm instruction so the compiler cannot constant-fold
//      or eliminate it; require the fcsr readback to be exactly
//      0x01 (NX set, frm still RNE).
//   4. csrs fcsr, (3<<5): set frm to RUP; require the readback
//      to be exactly 0x61, i.e. the accrued NX flag preserved
//      and frm == 3.
//   5. csrc fcsr, 0x1f: clear the fflags field; require the
//      readback to be exactly 0x60, i.e. frm preserved at RUP
//      while every flag bit went back to 0.
//   6. csrw fcsr, x0; require the readback to be exactly 0x00.
//
// The operands are loaded as bit patterns (0x3FF0000000000000
// for 1.0, 0x4008000000000000 for 3.0) moved into FP registers
// with fmv.d.x from integer registers, so no host floating point
// is involved in feeding the operation.
//
// A counting M-mode trap handler (ffi_trap.S) is installed as a
// safety net; the run requires its counter to stay 0. Every
// step is a check computed in code; the five fcsr readbacks are
// printed with their fflags/frm decodes, and a 64-bit FNV-1a
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
// blocks touch no memory. The fdiv.d's operand words move
// through integer registers only.

#include "../uart.h"

#define FS_MASK    (0x3UL << 13)   // mstatus bits 14:13
#define FS_INITIAL 1UL
#define SD_BIT     (1UL << 63)    // mstatus bit 63, summary of FS/XS/VS
#define MISA_FD    ((1UL << 5) | (1UL << 3))  // misa bits F (5) and D (3)

#define FFLAGS_ALL 0x1FUL         // fcsr bits 4:0: NV|DZ|OF|UF|NX
#define FRM_MASK   (0x7UL << 5)   // fcsr bits 7:5
#define FRM_RUP    3UL            // round toward +inf

#define ONE_BITS   0x3FF0000000000000UL  // 1.0 as a double bit pattern
#define THREE_BITS 0x4008000000000000UL  // 3.0 as a double bit pattern

#define CLINT_MTIME 0x0200bff8UL  // 64-bit mtime, UART-drain timebase

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// Trap record: mcause, mepc, mtval at trap entry, trap counter.
// mscratch points here; boot.S clears BSS so the counter starts at 0.
static volatile unsigned long ffi_trap[4];

extern void ffi_trap_entry(void);

static unsigned long read_fcsr(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, fcsr" : "=r"(v));
    return v;
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

// One volatile inexact divide. The volatile asm keeps the compiler
// from constant-folding the operation or dropping it. rm=RNE is
// written explicitly so the instruction in the binary is
// unambiguous (GAS defaults to rm=DYN on a bare fdiv.d).
static void div13(void) {
    __asm__ volatile(
        ".option push\n\t"
        ".option arch, +d\n\t"
        "fmv.d.x f2, %0\n\t"
        "fmv.d.x f3, %1\n\t"
        "fdiv.d f1, f2, f3, rne\n\t"   // the real instruction under test
        ".option pop\n\t"
        :
        : "r"(ONE_BITS), "r"(THREE_BITS));
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
    unsigned long s1, s2, s3, s4, s5;
    unsigned long words[8];
    unsigned long checksum;
    int checks = 0;

    uart_init();
    uart_puts("fcsr-field-independence: csrs/csrc on one fcsr field must not disturb the other\n");

    // Safety net: counting M-mode trap handler. No trap is expected;
    // the run requires the counter to stay 0.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)ffi_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)ffi_trap));
    __asm__ volatile("csrci mstatus, 8");  // MIE clear: no M-mode interrupt
    __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    uart_puts("setup: mtvec=");
    uart_put_hex(tv);
    uart_puts(" mie=");
    uart_put_hex(mie);
    uart_puts("\n");
    checks++; check((tv & ~3UL) == (unsigned long)ffi_trap_entry,
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

    // Step 1: csrw fcsr, x0 (frm=RNE, fflags clear); require the
    // readback to be exactly 0x00.
    __asm__ volatile("csrw fcsr, x0");
    s1 = read_fcsr();
    print_fcsr("step1", s1);
    checks++; check(s1 == 0x00UL, "fcsr readback != 0x00 after csrw clear");

    // Step 2: one inexact divide, 1.0/3.0, so fflags accrues NX
    // while frm stays RNE; require the readback to be exactly
    // 0x01.
    div13();
    s2 = read_fcsr();
    print_fcsr("step2", s2);
    checks++; check(s2 == 0x01UL,
                    "fcsr readback != 0x01 after the inexact fdiv.d");

    // Step 3: csrs fcsr, (3<<5) writes frm=RUP without touching
    // fflags; require the readback to be exactly 0x61, i.e. the
    // accrued NX flag preserved and frm == 3.
    __asm__ volatile("csrs fcsr, %0" :: "r"(FRM_RUP << 5));
    s3 = read_fcsr();
    print_fcsr("step3", s3);
    checks++; check(s3 == 0x61UL,
                    "fcsr readback != 0x61 after csrs set frm=RUP");
    checks++; check((s3 & FFLAGS_ALL) == 0x01UL,
                    "fflags field moved when only frm was written");
    checks++; check(((s3 >> 5) & 7UL) == FRM_RUP,
                    "frm != 3 (RUP) after csrs");

    // Step 4: csrc fcsr, 0x1f clears the fflags field without
    // touching frm; require the readback to be exactly 0x60,
    // i.e. frm preserved at RUP and every flag bit back to 0.
    __asm__ volatile("csrc fcsr, %0" :: "r"(FFLAGS_ALL));
    s4 = read_fcsr();
    print_fcsr("step4", s4);
    checks++; check(s4 == 0x60UL,
                    "fcsr readback != 0x60 after csrc clear fflags");
    checks++; check((s4 & FFLAGS_ALL) == 0UL,
                    "an fflags bit survived the csrc clear");
    checks++; check(((s4 >> 5) & 7UL) == FRM_RUP,
                    "frm != 3 (RUP) after the fflags clear");

    // Step 5: csrw fcsr, x0; require the readback to be exactly
    // 0x00.
    __asm__ volatile("csrw fcsr, x0");
    s5 = read_fcsr();
    print_fcsr("step5", s5);
    checks++; check(s5 == 0x00UL, "fcsr readback != 0x00 after csrw clear");

    // No trap may have fired during the run.
    uart_puts("traps: count=");
    uart_put_dec(ffi_trap[3]);
    uart_puts(" mcause=");
    uart_put_hex(ffi_trap[0]);
    uart_puts(" mepc=");
    uart_put_hex(ffi_trap[1]);
    uart_puts(" mtval=");
    uart_put_hex(ffi_trap[2]);
    uart_puts("\n");
    checks++; check(ffi_trap[3] == 0, "trap handler fired during the run");

    // fcsr read sequence: cleared, after fdiv.d, after csrs,
    // after csrc, cleared.
    uart_puts("sequence fcsr: s1=");
    uart_put_hex(s1);
    uart_puts(" s2=");
    uart_put_hex(s2);
    uart_puts(" s3=");
    uart_put_hex(s3);
    uart_puts(" s4=");
    uart_put_hex(s4);
    uart_puts(" s5=");
    uart_put_hex(s5);
    uart_puts("\n");

    // Checksum over the logged measurement words.
    words[0] = s1;
    words[1] = s2;
    words[2] = s3;
    words[3] = s4;
    words[4] = s5;
    words[5] = ffi_trap[3];
    words[6] = misa;
    words[7] = baseline;
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
