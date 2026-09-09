// csr_main.c: misa/marchid/mimpid readback with per-extension instruction probes.
//
// One mechanism: the hart's misa CSR is the ground-truth declaration of
// which ISA extensions the hart implements. This program reads misa,
// marchid and mimpid directly with csrr, decodes the MXL field and the
// 26-bit extension bitmap, prints the letters, then executes exactly
// one hand-written instruction characteristic of each reported
// extension under a trap handler that records mcause/mepc. A probe
// passes only if it executes without an illegal-instruction trap and
// produces the exact expected result; a trap prints mcause/mepc/mtval
// and fails the run.
//
// marchid/mimpid are read twice and the two reads must match (readback
// stability); their values are published as read, with no expectation
// hard-coded, since they vary across QEMU versions. S and U are
// privilege modes, not instructions, so they are reported but not
// probed. QEMU 8.2.2 virt reports imacfd plus H, S, U; only the letters
// actually present in the bitmap are probed.

#include "../uart.h"

extern void csr_trap_entry(void);

// Trap save area, laid out for csr_trap.S:
// [1]=t1 [2]=mcause [3]=mepc [4]=mtval [5]=resume pc [6]=seen flag.
// mscratch points here while a probe is armed.
volatile unsigned long csr_save[7];

// Aligned word used as the AMO target for the A-extension probe.
static volatile unsigned int amo_word __attribute__((aligned(4)));

static unsigned long csr_read_misa(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, misa" : "=r"(v));
    return v;
}

static unsigned long csr_read_marchid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, marchid" : "=r"(v));
    return v;
}

static unsigned long csr_read_mimpid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mimpid" : "=r"(v));
    return v;
}

static unsigned long csr_read_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

static unsigned long csr_read_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
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

// Print one probe's outcome. seen!=0 means the probe trapped; the trap
// handler recorded the exact trap registers in csr_save.
static void report_probe(const char *letter, const char *what) {
    uart_puts("probe ");
    uart_puts(letter);
    uart_puts(" (");
    uart_puts(what);
    uart_puts("): seen=");
    uart_put_dec(csr_save[6]);
    if (csr_save[6]) {
        uart_puts(" mcause=");
        uart_put_hex(csr_save[2]);
        uart_puts(" mepc=");
        uart_put_hex(csr_save[3]);
        uart_puts(" mtval=");
        uart_put_hex(csr_save[4]);
        uart_puts(" FAIL\n");
        fails++;
    } else {
        uart_puts(" PASS\n");
    }
}

// I: plain integer add. Result checked against the exact sum.
static void probe_i(void) {
    unsigned long r;
    __asm__ volatile(
        "la t0, 1f\n"
        "sd t0, 40(%1)\n"    // csr_save[5]: resume pc
        "sd zero, 48(%1)\n" // csr_save[6]: seen = 0
        "li t1, 100\n"
        "li t2, 23\n"
        "add %0, t1, t2\n"   // the probe
        "1:\n"
        : "=r"(r)
        : "r"(csr_save)
        : "t0", "t1", "t2", "memory");
    if (!csr_save[6])
        check(r == 123, "I probe: add result != 123");
    report_probe("I", "add");
}

// M: integer multiply. Result checked against the exact product.
static void probe_m(void) {
    unsigned long r;
    __asm__ volatile(
        "la t0, 1f\n"
        "sd t0, 40(%1)\n"
        "sd zero, 48(%1)\n"
        "li t1, 6\n"
        "li t2, 7\n"
        "mul %0, t1, t2\n"   // the probe
        "1:\n"
        : "=r"(r)
        : "r"(csr_save)
        : "t0", "t1", "t2", "memory");
    if (!csr_save[6])
        check(r == 42, "M probe: mul result != 42");
    report_probe("M", "mul");
}

// A: atomic word swap on an aligned RAM word. Old contents must come
// back in rd and the new value must land in memory.
static void probe_a(void) {
    unsigned long oldv, newv;
    amo_word = 0x12345678U;
    __asm__ volatile(
        "la t0, 1f\n"
        "sd t0, 40(%2)\n"    // csr_save[5]: resume pc
        "sd zero, 48(%2)\n" // csr_save[6]: seen = 0
        "la t1, amo_word\n"
        "li t2, 0xdeadbeef\n"
        "amoswap.w %0, t2, 0(t1)\n" // the probe
        "lwu %1, 0(t1)\n"         // zero-extending readback for the check
        "1:\n"
        : "=r"(oldv), "=r"(newv)
        : "r"(csr_save)
        : "t0", "t1", "t2", "memory");
    if (!csr_save[6]) {
        check(oldv == 0x12345678UL, "A probe: amoswap.w rd != old contents");
        check(newv == 0xdeadbeefUL, "A probe: memory != swapped value");
    }
    report_probe("A", "amoswap.w");
}

// C: one 16-bit compressed instruction. If the hart did not implement
// the C extension, the 16-bit parcel (bits[1:0] = 01, never a valid
// 32-bit opcode) would trap as illegal instruction.
static void probe_c(void) {
    unsigned long r;
    __asm__ volatile(
        ".option push\n"
        ".option rvc\n"
        "la t0, 1f\n"
        "sd t0, 40(%1)\n"
        "sd zero, 48(%1)\n"
        "li t1, 0\n"
        "c.addi t1, 5\n"     // the probe: 16-bit addi
        "mv %0, t1\n"
        ".option pop\n"
        "1:\n"
        : "=r"(r)
        : "r"(csr_save)
        : "t0", "t1", "memory");
    if (!csr_save[6])
        check(r == 5, "C probe: c.addi result != 5");
    report_probe("C", "c.addi");
}

// F: single-precision add of 1.0f + 2.0f, compared bit-exact to 3.0f.
static void probe_f(void) {
    unsigned long bits;
    __asm__ volatile(
        ".option push\n"
        ".option arch, +f\n"
        "la t0, 1f\n"
        "sd t0, 40(%1)\n"
        "sd zero, 48(%1)\n"
        "li t1, 0x3f800000\n"   // 1.0f
        "fmv.w.x fa0, t1\n"
        "li t1, 0x40000000\n"   // 2.0f
        "fmv.w.x fa1, t1\n"
        "fadd.s fa2, fa0, fa1\n" // the probe
        "fmv.x.w %0, fa2\n"
        ".option pop\n"
        "1:\n"
        : "=r"(bits)
        : "r"(csr_save)
        : "t0", "t1", "memory");
    if (!csr_save[6])
        check(bits == 0x40400000UL, "F probe: 1.0f+2.0f bits != 3.0f");
    report_probe("F", "fadd.s");
}

// D: double-precision add of 1.0 + 2.0, compared bit-exact to 3.0.
static void probe_d(void) {
    unsigned long bits;
    __asm__ volatile(
        ".option push\n"
        ".option arch, +d\n"
        "la t0, 1f\n"
        "sd t0, 40(%1)\n"
        "sd zero, 48(%1)\n"
        "li t1, 0x3ff0000000000000\n" // 1.0
        "fmv.d.x fa0, t1\n"
        "li t1, 0x4000000000000000\n" // 2.0
        "fmv.d.x fa1, t1\n"
        "fadd.d fa2, fa0, fa1\n"      // the probe
        "fmv.x.d %0, fa2\n"
        ".option pop\n"
        "1:\n"
        : "=r"(bits)
        : "r"(csr_save)
        : "t0", "t1", "memory");
    if (!csr_save[6])
        check(bits == 0x4008000000000000UL, "D probe: 1.0+2.0 bits != 3.0");
    report_probe("D", "fadd.d");
}

// H: hypervisor fence. In M-mode this is a legal no-op on a hart with
// the H extension; without H it traps as illegal instruction.
static void probe_h(void) {
    __asm__ volatile(
        ".option push\n"
        ".option arch, +h\n"
        "la t0, 1f\n"
        "sd t0, 40(%0)\n"
        "sd zero, 48(%0)\n"
        "hfence.gvma zero, zero\n" // the probe
        ".option pop\n"
        "1:\n"
        :
        : "r"(csr_save)
        : "t0", "memory");
    report_probe("H", "hfence.gvma");
}

int main(void) {
    unsigned long misa1, misa2, marchid1, marchid2, mimpid1, mimpid2;
    unsigned long bitmap, mxl;
    char letters[27];
    int nletters = 0;
    int i;

    uart_init();
    uart_puts("csr: misa readback and per-extension instruction probes\n");
    uart_puts("hart mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    // Install the trap vector (direct mode) and arm mscratch.
    __asm__ volatile("csrw mtvec, %0" :: "r"(csr_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(csr_save));

    // Read each identity CSR twice: the two reads must agree.
    misa1 = csr_read_misa();
    misa2 = csr_read_misa();
    marchid1 = csr_read_marchid();
    marchid2 = csr_read_marchid();
    mimpid1 = csr_read_mimpid();
    mimpid2 = csr_read_mimpid();

    uart_puts("misa    read1=");
    uart_put_hex(misa1);
    uart_puts(" read2=");
    uart_put_hex(misa2);
    uart_puts("\n");
    check(misa1 == misa2, "misa readback not stable");

    uart_puts("marchid read1=");
    uart_put_hex(marchid1);
    uart_puts(" read2=");
    uart_put_hex(marchid2);
    uart_puts("\n");
    check(marchid1 == marchid2, "marchid readback not stable");

    uart_puts("mimpid  read1=");
    uart_put_hex(mimpid1);
    uart_puts(" read2=");
    uart_put_hex(mimpid2);
    uart_puts("\n");
    check(mimpid1 == mimpid2, "mimpid readback not stable");

    // Decode: MXL in bits 62-63 (rv64), extension bitmap in bits 0-25.
    mxl = (misa1 >> 62) & 3UL;
    bitmap = misa1 & 0x3ffffffUL;
    uart_puts("misa MXL=");
    uart_put_dec(mxl);
    uart_puts(" (2 = RV64) bitmap=");
    uart_put_hex(bitmap);
    uart_puts("\n");
    check(mxl == 2, "misa MXL != 2 on this RV64 build");

    for (i = 0; i < 26; i++) {
        if (bitmap & (1UL << i))
            letters[nletters++] = (char)('A' + i);
    }
    letters[nletters] = '\0';
    uart_puts("misa extensions reported: ");
    uart_puts(letters);
    uart_puts("\n");

    // F/D probes need mstatus.FS set, else the FP instructions trap as
    // illegal even on a hart with F and D.
    __asm__ volatile("csrs mstatus, %0" :: "r"(0x6000UL));
    check((csr_read_mstatus() & 0x6000UL) == 0x6000UL,
          "mstatus.FS did not stick at Dirty");

    // One probe per reported letter.
    for (i = 0; i < 26; i++) {
        if (!(bitmap & (1UL << i)))
            continue;
        switch (i) {
        case 0:  probe_a(); break; // A
        case 2:  probe_c(); break; // C
        case 3:  probe_d(); break; // D
        case 5:  probe_f(); break; // F
        case 7:  probe_h(); break; // H
        case 8:  probe_i(); break; // I
        case 12: probe_m(); break; // M
        case 18: // S: privilege mode, not an instruction; reported only.
        case 20: // U: privilege mode, not an instruction; reported only.
            uart_puts("letter ");
            uart_putc((char)('A' + i));
            uart_puts(": privilege mode, no instruction probe\n");
            break;
        default:
            uart_puts("letter ");
            uart_putc((char)('A' + i));
            uart_puts(": reported by misa, no probe defined\n");
            break;
        }
    }

    if (fails == 0) {
        uart_puts("RESULT: PASS\n");
    } else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    return 0;
}
