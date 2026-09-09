// amo_main.c: misaligned LR/SC experiment.
//
// Installs an M-mode trap handler that records mcause/mepc/mtval on any
// trap, then issues three LR/SC sequences at fixed addresses derived
// from an aligned scratch buffer:
//
//   T1: lr.w at base+2 (not 4-byte aligned).
//   T2: aligned lr.w at base to set the reservation, then sc.w at
//       base+2 (not 4-byte aligned) storing 0x5A5A5A5A.
//   T3: misaligned lr.w at base+2 followed by sc.w to the same address
//       storing 0xA5A5A5A5 (run only if T1's lr completed, otherwise the
//       pair can never get past the lr).
//
// For each sequence the program reports whether a trap fired. On a trap
// it prints the exact mcause/mepc/mtval values and checks internal
// consistency (the misaligned-cause code, mtval equal to the faulting
// address, mepc equal to the faulting instruction's address, which is
// known exactly because each block uses .option norvc with a fixed
// layout). On transparent completion it prints the observed values: the
// loaded word for lr (checked against a byte-by-byte reconstruction),
// and the sc return value (0 = success, 1 = failure) with the affected
// memory read back to confirm what the store did.
//
// Whatever happens is reported honestly: a trap, a transparent
// completion, or an sc failure code are all measurements of how the
// emulated machine actually behaves. The checks only verify that the
// program's own accounting is consistent (right trap codes, right
// addresses, right round-trip bytes); they never assert which behavior
// the machine "must" show, because the spec leaves misaligned atomics
// implementation-defined and this runs on an emulator, not silicon.

#include "../uart.h"

extern void amo_trap_entry(void);

// Scratch area, aligned; the tests use fixed unaligned offsets from it.
static volatile unsigned char scratch[64] __attribute__((aligned(16)));

// Trap save area, laid out for amo_trap.S:
// [1]=t1 [2]=mcause [3]=mepc [4]=mtval [5]=resume pc [6]=seen flag
// [7]=result word (loaded value for lr, sc return code for sc,
//      valid only when seen==0).
volatile unsigned long amo_save[8];

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

// T1: misaligned lr.w at a known unaligned address. Exact layout:
// auipc (A), addi (A+4), lr.w at A+8, resume label at A+12. If the lr
// traps, the handler resumes at label 1 and mepc must be resume - 4.
// If it does not trap, t1 holds the loaded word, stored to amo_save[7].
static void test_lr_impl(unsigned long addr) {
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        "la t0, 1f\n"
        "sd t0, 40(%0)\n"      // amo_save[5]: resume pc
        "sd zero, 48(%0)\n"    // amo_save[6]: seen = 0
        "sd zero, 16(%0)\n"    // clear stale mcause/mepc/mtval
        "sd zero, 24(%0)\n"
        "sd zero, 32(%0)\n"
        "auipc t0, 0\n"        // A: address of this auipc
        "addi t0, %1, 0\n"     // t0 = misaligned address
        "lr.w t1, 0(t0)\n"     // the misaligned lr, at A+8
        "1:\n"
        "sd t1, 56(%0)\n"      // amo_save[7]: loaded value (if no trap)
        ".option pop\n"
        :
        : "r"(amo_save), "r"(addr)
        : "t0", "t1", "memory");
}

// T2: aligned lr.w at base to establish a reservation, then misaligned
// sc.w at base+2. The sc is the last instruction before label 1, so if
// the sc traps, mepc must be resume - 4. The aligned lr should not trap;
// if it did, the mepc check would catch the accounting error.
static void test_sc_misaligned_impl(unsigned long base, unsigned int val) {
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        "la t0, 1f\n"
        "sd t0, 40(%0)\n"      // amo_save[5]: resume pc
        "sd zero, 48(%0)\n"    // amo_save[6]: seen = 0
        "sd zero, 16(%0)\n"    // clear stale mcause/mepc/mtval
        "sd zero, 24(%0)\n"
        "sd zero, 32(%0)\n"
        "auipc t0, 0\n"
        "addi t0, %1, 0\n"     // t0 = base (aligned)
        "lr.w t1, 0(t0)\n"     // aligned lr: reservation set here
        "addi t0, t0, 2\n"     // t0 = base+2 (misaligned)
        "li t2, %2\n"          // t2 = value to store
        "sc.w t1, t2, 0(t0)\n" // the misaligned sc, just before 1:
        "1:\n"
        "sd t1, 56(%0)\n"      // amo_save[7]: sc return (if no trap)
        ".option pop\n"
        :
        : "r"(amo_save), "r"(base), "i"(val)
        : "t0", "t1", "t2", "memory");
}

// T3: misaligned lr.w at addr followed by sc.w to the same address.
// Same shape as T2: the sc is the last instruction before label 1, so a
// sc trap gives mepc == resume - 4. Only run when T1's lr completed.
static void test_pair_impl(unsigned long addr, unsigned int val) {
    __asm__ volatile(
        ".option push\n"
        ".option norvc\n"
        "la t0, 1f\n"
        "sd t0, 40(%0)\n"      // amo_save[5]: resume pc
        "sd zero, 48(%0)\n"    // amo_save[6]: seen = 0
        "sd zero, 16(%0)\n"    // clear stale mcause/mepc/mtval
        "sd zero, 24(%0)\n"
        "sd zero, 32(%0)\n"
        "auipc t0, 0\n"
        "addi t0, %1, 0\n"     // t0 = misaligned address
        "lr.w t1, 0(t0)\n"     // misaligned lr
        "li t2, %2\n"          // t2 = value to store
        "sc.w t1, t2, 0(t0)\n" // misaligned sc, just before 1:
        "1:\n"
        "sd t1, 56(%0)\n"      // amo_save[7]: sc return (if no trap)
        ".option pop\n"
        :
        : "r"(amo_save), "r"(addr), "i"(val)
        : "t0", "t1", "t2", "memory");
}

static void report(const char *name) {
    uart_puts(name);
    uart_puts(": seen=");
    uart_put_dec(amo_save[6]);
    uart_puts(" mcause=");
    uart_put_hex(amo_save[2]);
    uart_puts(" mepc=");
    uart_put_hex(amo_save[3]);
    uart_puts(" mtval=");
    uart_put_hex(amo_save[4]);
    uart_puts(" resume=");
    uart_put_hex(amo_save[5]);
    uart_puts("\n");
}

static void report_bytes(unsigned long addr) {
    volatile unsigned char *p = (volatile unsigned char *)addr;
    int i;
    uart_puts("bytes at addr..addr+3 = ");
    for (i = 0; i < 4; i++) {
        uart_put_hex((unsigned long)p[i]);
        uart_puts(" ");
    }
    uart_puts("\n");
}

int main(void) {
    unsigned long base = (unsigned long)scratch;
    unsigned long addr, expected, loaded, scr;
    int i, lr_trapped;

    uart_init();
    uart_puts("amo: misaligned LR/SC experiment\n");
    uart_puts("hart mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    // Install the trap vector (direct mode) and arm mscratch.
    __asm__ volatile("csrw mtvec, %0" :: "r"(amo_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(amo_save));
    uart_puts("trap vector installed at ");
    uart_put_hex((unsigned long)amo_trap_entry);
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

    // T1: misaligned lr.w at base+2.
    addr = base + 2;
    uart_puts("T1 lr.w address=");
    uart_put_hex(addr);
    uart_puts(" (base+2, not 4-byte aligned)\n");
    test_lr_impl(addr);
    report("T1 lr");
    lr_trapped = (amo_save[6] == 1);
    if (lr_trapped) {
        uart_puts("T1 lr TRAPPED: mcause=");
        uart_put_hex(amo_save[2]);
        uart_puts(" (4 = load address misaligned)\n");
        check(amo_save[2] == 4, "T1 trap mcause != 4 (load address misaligned)");
        check(amo_save[4] == addr, "T1 trap mtval != faulting address");
        check(amo_save[3] == amo_save[5] - 4,
              "T1 trap mepc != address of faulting lr.w");
    } else {
        loaded = amo_save[7];
        expected = ref_lw4(addr);
        uart_puts("T1 lr completed transparently: loaded=");
        uart_put_hex(loaded);
        uart_puts(" byte-wise reference=");
        uart_put_hex(expected);
        uart_puts("\n");
        // lr.w loads a 32-bit word and sign-extends bit 31.
        check(loaded == (unsigned long)(long)(int)expected,
              "T1 transparent lr value != sign-extended byte-wise reference");
        check(amo_save[2] == 0, "T1 unexpected mcause without trap");
    }

    // T2: aligned lr at base, then misaligned sc at base+2 (0x5A5A5A5A).
    uart_puts("T2: aligned lr.w at ");
    uart_put_hex(base);
    uart_puts(", then sc.w at ");
    uart_put_hex(base + 2);
    uart_puts(" storing 0x5a5a5a5a\n");
    test_sc_misaligned_impl(base, 0x5A5A5A5AU);
    report("T2 sc");
    if (amo_save[6] == 1) {
        uart_puts("T2 sc TRAPPED: mcause=");
        uart_put_hex(amo_save[2]);
        uart_puts(" (6 = store/AMO address misaligned)\n");
        check(amo_save[2] == 6, "T2 trap mcause != 6 (store/AMO address misaligned)");
        check(amo_save[4] == base + 2, "T2 trap mtval != faulting address");
        check(amo_save[3] == amo_save[5] - 4,
              "T2 trap mepc != address of faulting sc.w");
    } else {
        scr = amo_save[7];
        uart_puts("T2 sc completed transparently: sc return=");
        uart_put_dec(scr);
        uart_puts(" (0=success, 1=failure)\n");
        report_bytes(base + 2);
        if (scr == 0) {
            // Store landed: exactly bytes base+2..base+5 changed.
            check(ref_lw4(base + 2) == 0x5A5A5A5AU,
                  "T2 sc returned 0 but memory does not hold the stored value");
            check(((volatile unsigned char *)base)[1] == 0xC3,
                  "T2 sc clobbered byte below the store range");
            check(((volatile unsigned char *)base)[6] == 0xB2,
                  "T2 sc clobbered byte above the store range");
        } else if (scr == 1) {
            // Reservation dropped (or never valid for this address):
            // memory must be unchanged.
            uart_puts("T2 sc returned 1 (failure): checking memory unchanged\n");
            check(((volatile unsigned char *)(base + 2))[0] == 0xB2 &&
                  ((volatile unsigned char *)(base + 2))[1] == 0xA1 &&
                  ((volatile unsigned char *)(base + 2))[2] == 0xD5 &&
                  ((volatile unsigned char *)(base + 2))[3] == 0xC3,
                  "T2 sc returned 1 but memory was modified");
        } else {
            check(0, "T2 sc return not 0 or 1");
        }
    }

    // T3: misaligned lr/sc pair at base+2, only if T1's lr completed.
    if (lr_trapped) {
        uart_puts("T3 skipped: T1's lr.w trapped, so a misaligned pair can\n");
        uart_puts("never reach the sc; the lr trap is the finding.\n");
    } else {
        uart_puts("T3: misaligned lr.w then sc.w at ");
        uart_put_hex(base + 2);
        uart_puts(" storing 0xa5a5a5a5\n");
        test_pair_impl(base + 2, 0xA5A5A5A5U);
        report("T3 pair");
        if (amo_save[6] == 1) {
            uart_puts("T3 pair TRAPPED: mcause=");
            uart_put_hex(amo_save[2]);
            uart_puts("\n");
            // The sc is the last instruction before the resume label, so
            // a trap on the sc gives mepc == resume - 4 (mcause 6). A
            // trap on the lr (mcause 4) lands earlier; either is recorded
            // honestly and the mepc/resume relation says which.
            if (amo_save[3] == amo_save[5] - 4) {
                uart_puts("trap on sc.w\n");
                check(amo_save[2] == 6, "T3 sc trap mcause != 6");
                check(amo_save[4] == base + 2, "T3 sc trap mtval != faulting address");
            } else {
                uart_puts("trap on lr.w\n");
                check(amo_save[2] == 4, "T3 lr trap mcause != 4");
                check(amo_save[4] == base + 2, "T3 lr trap mtval != faulting address");
            }
        } else {
            scr = amo_save[7];
            uart_puts("T3 pair completed transparently: sc return=");
            uart_put_dec(scr);
            uart_puts("\n");
            report_bytes(base + 2);
            if (scr == 0)
                check(ref_lw4(base + 2) == 0xA5A5A5A5U,
                      "T3 sc returned 0 but memory does not hold the stored value");
            else
                check(scr == 1, "T3 sc return not 0 or 1");
        }
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
