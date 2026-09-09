// mis_main.c: misaligned load/store experiment.
//
// Installs an M-mode trap handler that records mcause/mepc/mtval on any
// trap, then issues a misaligned lw (address not 4-byte aligned) and a
// misaligned sw, each at a fixed address derived from an aligned scratch
// buffer. For each access the program reports whether a trap fired. If a
// trap fired, it reports the exact mcause/mepc/mtval values. If no trap
// fired, the access completed transparently and the program reports the
// loaded value (checked against a byte-by-byte reconstruction of the
// same memory) and verifies the stored value round-trips exactly.
//
// Whatever happens is reported honestly: a transparent completion is a
// measurement of how the emulated machine actually behaves, not a
// failure of the experiment.

#include "../uart.h"

extern void mis_trap_entry(void);

// Scratch area, aligned; the tests use fixed unaligned offsets from it.
static volatile unsigned char scratch[64] __attribute__((aligned(16)));

// Trap save area, laid out for mis_trap.S:
// [1]=t1 [2]=mcause [3]=mepc [4]=mtval [5]=resume pc [6]=seen flag
// [7]=loaded value (lw test only, valid when seen==0).
volatile unsigned long mis_save[8];

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long csr_read_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

// Byte-wise reference: read 4 bytes at addr and assemble little-endian.
static unsigned long ref_lw4(unsigned long addr) {
    volatile unsigned char *p = (volatile unsigned char *)addr;
    return (unsigned long)p[0]
         | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16)
         | ((unsigned long)p[3] << 24);
}

// Misaligned lw at a known unaligned address. One asm block so the layout
// is exact: `.option norvc` forces every instruction in the trap window
// to its 4-byte form (the assembler would otherwise fold
// `addi t0, %1, 0` into the 2-byte c.mv, verified in the disassembly),
// so the sequence is auipc (4), addi (4), lw (4), resume label. If the lw
// traps, the handler resumes at label 1 with the seen flag set; mepc of
// the faulting lw must then equal resume_pc - 8. If it does not trap,
// control falls through and the loaded value is stored to mis_save[7].
static void test_lw_impl(unsigned long addr) {
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        "la t0, 1f\n"
        "sd t0, 40(%0)\n"      // mis_save[5]: resume pc
        "sd zero, 48(%0)\n"    // mis_save[6]: seen = 0
        "auipc t0, 0\n"        // A: address of this auipc
        "addi t0, %1, 0\n"     // t0 = misaligned address
        "lw t1, 0(t0)\n"       // the misaligned load, at A+8
        "1:\n"
        "sd t1, 56(%0)\n"      // mis_save[7]: loaded value (if no trap)
        ".option pop\n"
        :
        : "r"(mis_save), "r"(addr)
        : "t0", "t1", "memory");
}

// Misaligned sw at a known unaligned address. Same exact-layout
// construction; the faulting sw sits at A+12 (auipc + addi + addi).
static void test_sw_impl(unsigned long addr, unsigned int val) {
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        "la t0, 1f\n"
        "sd t0, 40(%0)\n"      // mis_save[5]: resume pc
        "sd zero, 48(%0)\n"    // mis_save[6]: seen = 0
        "auipc t0, 0\n"        // A: address of this auipc
        "addi t0, %1, 0\n"     // t0 = misaligned address
        "addi t1, %2, 0\n"     // t1 = value to store
        "sw t1, 0(t0)\n"       // the misaligned store, at A+12
        "1:\n"
        ".option pop\n"
        :
        : "r"(mis_save), "r"(addr), "r"(val)
        : "t0", "t1", "memory");
}

static void report(const char *name) {
    uart_puts(name);
    uart_puts(": seen=");
    uart_put_dec(mis_save[6]);
    uart_puts(" mcause=");
    uart_put_hex(mis_save[2]);
    uart_puts(" mepc=");
    uart_put_hex(mis_save[3]);
    uart_puts(" mtval=");
    uart_put_hex(mis_save[4]);
    uart_puts(" resume=");
    uart_put_hex(mis_save[5]);
    uart_puts("\n");
}

int main(void) {
    unsigned long base = (unsigned long)scratch;
    unsigned long addr_lw, addr_sw, expected, loaded;
    unsigned int sval = 0x12345678U;
    volatile unsigned char *p;
    int i;

    uart_init();
    uart_puts("misaligned: misaligned load/store experiment\n");
    uart_puts("hart mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    // Install the trap vector (direct mode) and arm mscratch.
    __asm__ volatile("csrw mtvec, %0" :: "r"(mis_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(mis_save));
    uart_puts("trap vector installed at ");
    uart_put_hex((unsigned long)mis_trap_entry);
    uart_puts("\n");

    // Control: the buffer is good RAM with aligned accesses.
    for (i = 0; i < 8; i++)
        ((volatile unsigned int *)base)[i] = 0xA1B2C3D4U + (unsigned int)i;
    for (i = 0; i < 8; i++)
        check(((volatile unsigned int *)base)[i] == 0xA1B2C3D4U + (unsigned int)i,
              "aligned access to scratch failed");
    uart_puts("control: aligned load/store on scratch: ok\n");
    uart_puts("scratch base=");
    uart_put_hex(base);
    uart_puts("\n");

    // Test 1: misaligned lw at base+1 (address not 4-byte aligned).
    addr_lw = base + 1;
    uart_puts("lw address=");
    uart_put_hex(addr_lw);
    uart_puts(" (base+1, not 4-byte aligned)\n");
    test_lw_impl(addr_lw);
    report("lw");
    if (mis_save[6] == 1) {
        uart_puts("lw TRAPPED: mcause=");
        uart_put_hex(mis_save[2]);
        uart_puts(" (4 = load address misaligned)\n");
        check(mis_save[2] == 4, "lw trap mcause != 4 (load address misaligned)");
        check(mis_save[4] == addr_lw, "lw trap mtval != faulting address");
        check(mis_save[3] == mis_save[5] - 8,
              "lw trap mepc != address of faulting lw");
    } else {
        loaded = mis_save[7];
        expected = ref_lw4(addr_lw);
        uart_puts("lw completed transparently: loaded=");
        uart_put_hex(loaded);
        uart_puts(" byte-wise reference=");
        uart_put_hex(expected);
        uart_puts("\n");
        // lw is a signed word load: it sign-extends bit 31, so compare
        // against the sign-extended 32-bit reference.
        check(loaded == (unsigned long)(long)(int)expected,
              "transparent lw value != sign-extended byte-wise reference");
        check(mis_save[2] == 0, "unexpected mcause without trap");
    }

    // Test 2: misaligned sw at base+2 storing 0x12345678, verified by
    // reading the 4 affected bytes back individually.
    addr_sw = base + 2;
    uart_puts("sw address=");
    uart_put_hex(addr_sw);
    uart_puts(" (base+2, not 4-byte aligned) value=0x12345678\n");
    test_sw_impl(addr_sw, sval);
    report("sw");
    if (mis_save[6] == 1) {
        uart_puts("sw TRAPPED: mcause=");
        uart_put_hex(mis_save[2]);
        uart_puts(" (6 = store/AMO address misaligned)\n");
        check(mis_save[2] == 6, "sw trap mcause != 6 (store/AMO address misaligned)");
        check(mis_save[4] == addr_sw, "sw trap mtval != faulting address");
        check(mis_save[3] == mis_save[5] - 4,
              "sw trap mepc != address of faulting sw");
    } else {
        p = (volatile unsigned char *)addr_sw;
        uart_puts("sw completed transparently: bytes read back = ");
        for (i = 0; i < 4; i++) {
            uart_put_hex((unsigned long)p[i]);
            uart_puts(" ");
        }
        uart_puts("(expect 78 56 34 12)\n");
        check(p[0] == 0x78 && p[1] == 0x56 && p[2] == 0x34 && p[3] == 0x12,
              "transparent sw bytes != stored value");
        // Neighboring bytes must be untouched: the store touched exactly
        // bytes base+2..base+5, so byte base+1 still holds 0xC3 from the
        // aligned control fill (0xA1B2C3D4 at base+0).
        check(((volatile unsigned char *)base)[1] == 0xC3,
              "transparent sw clobbered neighboring byte");
    }

    if (fails == 0)
        uart_puts("RESULT: PASS\n");
    else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
