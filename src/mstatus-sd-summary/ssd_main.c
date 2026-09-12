// ssd_main.c: mstatus.SD tracks the FS Dirty state (backlog item
// "riscv mstatus-sd-summary").
//
// Mechanism under test: mstatus.SD is bit 63, a read-only summary bit
// that is 1 exactly when FS (bits 14:13) or XS (bits 17:16) reads
// Dirty (field value 3). The run, all in M-mode on QEMU 8.2.2 virt:
//
//   1. reads the boot mstatus word (expect FS=Off, SD=0);
//   2. writes FS=Off, reads back FS==0 and SD==0;
//   3. writes FS=Initial, reads back FS==2 and SD==0 (SD is 1 only
//      for Dirty, not for merely enabled);
//   4. executes fadd.d f0,f1,f2 on 1.5 and 2.25 loaded as integer
//      bit patterns via fmv.d.x, then reads back FS==3, SD==1, and
//      f0 holding exactly 3.75 (0x400e000000000000), proving the
//      FPU really ran and dirtied the FP state;
//   5. writes FS=Clean, reads back FS==1 and SD==0 again;
//   6. forces mstatus bit 63 to 1 with csrs and reads back: SD must
//      still read 0, proving software cannot set the summary bit;
//      it only reflects FS/XS state. (If the emulator ever set it,
//      the readback is printed and the run fails honestly; nothing
//      about this step is assumed in advance.)
//   7. restores the exact boot mstatus word and requires the
//      readback to match bit-for-bit.
//
// All mstatus writes are read-modify-write (csrc/csrs) preserving
// the other bits, except the deliberate bit-63 probe and the final
// explicit restore. The FP instructions are emitted under
// .option arch,+d because the module builds with
// -march=rv64imac_zicsr; the compiler cannot allocate FP registers,
// so no clobbers are needed. A counting M-mode trap handler
// (ssd_trap.S) is installed as hygiene; the run requires its counter
// to stay 0, and on PASS the machine shuts down through the virt
// test-device finisher (QEMU exits 0). On FAIL the hart parks in a
// wfi loop without touching the finisher, so under the harness's
// `timeout` a FAIL is the timeout exit status (124) plus the RESULT
// line.

#include "../uart.h"

#define FS_MASK   (0x3UL << 13)  // mstatus bits 14:13
#define FS_OFF    0UL
#define FS_CLEAN  1UL
#define FS_INIT   2UL
#define FS_DIRTY  3UL
#define SD_BIT    (1UL << 63)    // mstatus bit 63, read-only summary

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// Double-precision bit patterns, written as integers so the
// compiler's integer-only march never sees floating point.
#define DBL_1_5  0x3FF8000000000000UL  // 1.5
#define DBL_2_25 0x4002000000000000UL  // 2.25
#define DBL_3_75 0x400E000000000000UL  // 1.5 + 2.25

// Trap record, written by ssd_trap.S: [0] parked t1, [1] mcause,
// [2] mepc, [3] mtval, [4] trap counter. boot.S clears BSS. Not
// static: the trap entry references it by name.
volatile unsigned long ssd_save[5];

extern void ssd_trap_entry(void);

static unsigned long read_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static int fs_of(unsigned long v) {
    return (int)((v >> 13) & 3UL);
}

static int sd_of(unsigned long v) {
    return (int)((v >> 63) & 1UL);
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

// Write the FS field to the given value (0..3) by read-modify-write,
// preserving every other mstatus bit.
static void write_fs(unsigned long v) {
    __asm__ volatile("csrc mstatus, %0" :: "r"(FS_MASK));
    __asm__ volatile("csrs mstatus, %0" :: "r"((v & 3UL) << 13));
}

int main(void) {
    unsigned long tv, boot;
    unsigned long w_off, w_init, w_dirty, w_clean, w_probe, w_restored;
    unsigned long b1 = DBL_1_5, b2 = DBL_2_25, f0bits = 0;
    unsigned long words[9], checksum;

    uart_init();
    uart_puts("mstatus-sd-summary: mstatus.SD tracks the FS Dirty state\n");

    // Counting M-mode trap handler as hygiene; no trap is expected.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)ssd_trap_entry));
    __asm__ volatile("csrci mstatus, 8");  // MIE clear: no M-mode interrupt
    __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
    uart_puts("setup: mtvec=");
    uart_put_hex(tv);
    uart_puts("\n");

    // 1. Boot baseline: FS must read Off, SD must read 0.
    boot = read_mstatus();
    uart_puts("boot: mstatus=");
    uart_put_hex(boot);
    uart_puts(" FS=");
    uart_put_dec((unsigned long)fs_of(boot));
    uart_puts(" SD=");
    uart_put_dec((unsigned long)sd_of(boot));
    uart_puts(" (expect FS=0 SD=0)\n");
    check(fs_of(boot) == (int)FS_OFF, "boot mstatus FS field != 0 (Off)");
    check(sd_of(boot) == 0, "boot mstatus SD != 0");

    // 2. Explicitly write FS=Off; SD must stay 0.
    write_fs(FS_OFF);
    w_off = read_mstatus();
    uart_puts("fs-off: mstatus=");
    uart_put_hex(w_off);
    uart_puts(" FS=");
    uart_put_dec((unsigned long)fs_of(w_off));
    uart_puts(" SD=");
    uart_put_dec((unsigned long)sd_of(w_off));
    uart_puts(" (expect FS=0 SD=0)\n");
    check(fs_of(w_off) == (int)FS_OFF, "FS did not read back Off");
    check(sd_of(w_off) == 0, "SD read 1 with FS=Off");

    // 3. Write FS=Initial (2): enabled but not dirty, so SD stays 0.
    write_fs(FS_INIT);
    w_init = read_mstatus();
    uart_puts("fs-initial: mstatus=");
    uart_put_hex(w_init);
    uart_puts(" FS=");
    uart_put_dec((unsigned long)fs_of(w_init));
    uart_puts(" SD=");
    uart_put_dec((unsigned long)sd_of(w_init));
    uart_puts(" (expect FS=2 SD=0)\n");
    check(fs_of(w_init) == (int)FS_INIT, "FS did not read back Initial");
    check(sd_of(w_init) == 0, "SD read 1 with FS=Initial (not Dirty)");

    // 4. fadd.d on 1.5 and 2.25 must execute, move FS to Dirty, and
    // set SD to 1. Operands load as integer bit patterns via fmv.d.x;
    // f0 moves back out with fmv.x.d for a pure bit check.
    __asm__ volatile(
        ".option push\n\t"
        ".option arch, +d\n\t"
        ".option norvc\n\t"
        "fmv.d.x f1, %1\n\t"        // f1 = 1.5
        "fmv.d.x f2, %2\n\t"        // f2 = 2.25
        "fadd.d f0, f1, f2\n\t"     // must execute, not trap
        "fmv.x.d %0, f0\n\t"        // f0 bits -> integer
        ".option pop\n\t"
        : "=r"(f0bits)
        : "r"(b1), "r"(b2)
        : "memory");
    w_dirty = read_mstatus();
    uart_puts("after-fadd: mstatus=");
    uart_put_hex(w_dirty);
    uart_puts(" FS=");
    uart_put_dec((unsigned long)fs_of(w_dirty));
    uart_puts(" SD=");
    uart_put_dec((unsigned long)sd_of(w_dirty));
    uart_puts(" f0bits=");
    uart_put_hex(f0bits);
    uart_puts("\n");
    check(fs_of(w_dirty) == (int)FS_DIRTY,
          "fadd.d did not move FS to Dirty");
    check(sd_of(w_dirty) == 1,
          "SD did not read 1 with FS=Dirty");
    check(f0bits == DBL_3_75,
          "fadd.d 1.5+2.25 did not produce 3.75 in f0");

    // 5. Write FS=Clean (1): the dirtiness clears, so SD must fall
    // back to 0.
    write_fs(FS_CLEAN);
    w_clean = read_mstatus();
    uart_puts("fs-clean: mstatus=");
    uart_put_hex(w_clean);
    uart_puts(" FS=");
    uart_put_dec((unsigned long)fs_of(w_clean));
    uart_puts(" SD=");
    uart_put_dec((unsigned long)sd_of(w_clean));
    uart_puts(" (expect FS=1 SD=0)\n");
    check(fs_of(w_clean) == (int)FS_CLEAN, "FS did not read back Clean");
    check(sd_of(w_clean) == 0, "SD read 1 after FS=Clean cleared it");

    // 6. Read-only probe: with FS still Clean, force bit 63 to 1 and
    // read back. SD must still read 0: software cannot set the
    // summary bit, it only reflects FS/XS state. The readback is
    // printed verbatim; if the emulator ever honoured the write,
    // the check fails honestly and the log shows what happened.
    __asm__ volatile("csrs mstatus, %0" :: "r"(SD_BIT));
    w_probe = read_mstatus();
    uart_puts("sd-probe: mstatus=");
    uart_put_hex(w_probe);
    uart_puts(" SD=");
    uart_put_dec((unsigned long)sd_of(w_probe));
    uart_puts(" FS=");
    uart_put_dec((unsigned long)fs_of(w_probe));
    uart_puts(" (expect SD=0 FS=1)\n");
    check(sd_of(w_probe) == 0,
          "SD read 1 after a forced bit-63 write (summary bit is read-only)");
    check(fs_of(w_probe) == (int)FS_CLEAN,
          "FS changed across the SD probe write");

    // 7. Restore the exact boot mstatus word, bit-for-bit.
    __asm__ volatile("csrw mstatus, %0" :: "r"(boot));
    w_restored = read_mstatus();
    uart_puts("restore: mstatus=");
    uart_put_hex(w_restored);
    uart_puts(" (expect boot word)\n");
    check(w_restored == boot,
          "explicit csrw did not restore the boot mstatus word");

    // 8. No trap may have fired anywhere in the run.
    check(ssd_save[4] == 0, "a trap fired during the run");
    uart_puts("traps=");
    uart_put_dec(ssd_save[4]);
    uart_puts("\n");

    // Checksum over the measured words: boot mstatus, the Off,
    // Initial, post-fadd, Clean, and probe readbacks, the f0 bits,
    // the restored word, and the trap count. No host data enters.
    words[0] = boot;
    words[1] = w_off;
    words[2] = w_init;
    words[3] = w_dirty;
    words[4] = f0bits;
    words[5] = w_clean;
    words[6] = w_probe;
    words[7] = w_restored;
    words[8] = ssd_save[4];
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
        volatile unsigned long i;
        for (i = 0; i < 200000UL; i++)
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
