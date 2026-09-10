// pns_main.c: PMP NAPOT size-decoding test (backlog item 118).
//
// Programs two locked no-access PMP entries in NAPOT (naturally aligned
// power-of-two) mode over one 64 KiB scratch region, at two different
// sizes, and proves by boundary probes that the fault/no-fault boundary
// moves exactly as the pmpaddr trailing-ones encoding predicts.
//
// NAPOT encoding under test: pmpaddr = (base >> 2) | ((size >> 3) - 1).
// The low run of 1 bits encodes the size as size = 8 << (trailing ones):
//   4 KiB  -> pmpaddr = (base>>2) | 0x1FF   (9 trailing ones)
//   64 KiB -> pmpaddr = (base>>2) | 0x1FFF  (13 trailing ones)
//
// Entry 0 (programmed first, phase A): 4 KiB deny over [base, base+4K).
// Entry 1 (programmed second, phase B): 64 KiB deny over [base, base+64K).
// Both carry L=1. The lock bit is required because this test runs in
// M-mode and unlocked PMP entries are not checked against M-mode
// accesses. Locking also freezes each entry's pmpaddr, which is why the
// two sizes live in two entries (entry 1 is programmed after the phase-A
// probes); the lowest-numbered matching entry wins, and both deny, so
// the 64 KiB entry governs once it exists.
//
// Verified in the M-mode trap handler (records mcause/mepc/mtval):
//   Phase A (4 KiB encoding only):
//     (a) lbu at base+0x2000, inside a 64 KiB window but outside the 4 KiB
//         window, completes with no trap and returns the pattern byte.
//     (b) lbu at base+0xFFF, the last byte inside, traps: mcause 5 (load
//         access fault), mepc exactly the faulting instruction, mtval
//         exactly the faulting address.
//     (c) lbu at base+0x1000, the first byte outside, completes with no
//         trap and returns the pattern byte.
//   Phase B (64 KiB encoding added):
//     (d) lbu at base+0x2000, the same address as (a), now traps with
//         mcause 5 and exact mepc/mtval: the boundary moved with the size.
//     (e) lbu at base+0xFFFF, the last byte inside the large region,
//         traps with mcause 5 and exact mepc/mtval.
//     (f) lbu at base+0x10000, the first byte outside the large region,
//         completes with no trap (byte-exact boundary evidence; the value
//         is not checked because this build links the `fails` counter at
//         exactly that address, see Controls).
//   A dedicated sentinel byte outside the region is probed in phase B
//   with no trap and its 0xA5 value read back, proving outside addresses
//   are genuinely readable.
//
// Controls: the scratch region is pattern-filled and read back before
// any PMP programming (proving the probed addresses are good RAM), a
// sentinel byte in a dedicated static outside the region is written and
// read back, and a buffer elsewhere in .bss stays accessible after
// programming (proving the entries are narrow and the faults come from
// the PMP check). Note: no data is ever written at base+0x10000 itself;
// in this build the link order places the `fails` counter exactly there,
// so the first-byte-outside probe is verified by no-trap alone while the
// dedicated sentinel proves outside-region readability with a real value.

#include "../uart.h"

extern void pns_trap_entry(void);

// 64 KiB scratch, 64 KiB aligned: the region under test.
static volatile unsigned char scratch[65536] __attribute__((aligned(65536)));
// Probe buffer placed outside the PMP region (elsewhere in .bss).
static volatile unsigned long probe_outside[4];
// Sentinel byte in a dedicated static outside the PMP region; proves an
// outside address is genuinely readable (returns a real value), not
// merely non-faulting.
static volatile unsigned char far_sentinel;

// Trap save area, laid out for pns_trap.S:
// [1]=loaded byte / saved t1 [2]=mcause [3]=mepc [4]=mtval
// [5]=resume pc [6]=seen flag. mscratch points here while armed.
volatile unsigned long pns_save[7];

static void csr_write_pmpaddr0(unsigned long v) {
    __asm__ volatile("csrw pmpaddr0, %0" :: "r"(v));
}

static void csr_write_pmpaddr1(unsigned long v) {
    __asm__ volatile("csrw pmpaddr1, %0" :: "r"(v));
}

static void csr_write_pmpcfg0(unsigned long v) {
    __asm__ volatile("csrw pmpcfg0, %0" :: "r"(v));
}

static unsigned long csr_read_pmpaddr0(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpaddr0" : "=r"(v));
    return v;
}

static unsigned long csr_read_pmpaddr1(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpaddr1" : "=r"(v));
    return v;
}

static unsigned long csr_read_pmpcfg0(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(v));
    return v;
}

static unsigned long csr_read_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

// Config byte: L (0x80) + A=NAPOT (0x18) + R=W=X=0. Locked no-access.
#define PMP_L_NAPOT_DENY 0x98UL
// NAPOT size fields: (size >> 3) - 1, the trailing-ones run.
#define NAPOT_SIZE_4K  0x1FFUL    // 9 trailing ones: 8 << 9 = 4096
#define NAPOT_SIZE_64K 0x1FFFUL   // 13 trailing ones: 8 << 13 = 65536

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// Attempt an lbu at the given address; the trap handler resumes at
// label 1. One asm block so the layout is exact: the auipc is 4 bytes
// (never compressed), the lbu is 4 bytes (t1 is not a compressible
// register), then a 4-byte sd spills the loaded byte, then the resume
// label. So mepc of a faulting lbu must equal resume_pc - 8, checked
// below and confirmed against the disassembly. The resume address is
// taken inside the asm with `la t0, 1f` against a numeric local label,
// resolved exactly by the assembler (C &&label does not pin the position
// at -O2 without a computed goto).
static void test_lbu(unsigned long addr, int expect_trap,
                     unsigned long expect_val, int check_val) {
    __asm__ volatile(
        "la t0, 1f\n"
        "sd t0, 40(%0)\n"      // pns_save[5]: resume pc
        "sd zero, 48(%0)\n"    // pns_save[6]: seen = 0
        "auipc t0, 0\n"        // A: address of this auipc
        "lbu t1, 0(%1)\n"      // probed load, at A+4
        "sd t1, 8(%0)\n"       // pns_save[1]: loaded byte (no-trap path)
        "1:\n"
        :
        : "r"(pns_save), "r"(addr)
        : "t0", "t1", "memory");

    uart_puts("lbu @");
    uart_put_hex(addr);
    uart_puts(": seen=");
    uart_put_dec(pns_save[6]);
    uart_puts(" mcause=");
    uart_put_hex(pns_save[2]);
    uart_puts(" mepc=");
    uart_put_hex(pns_save[3]);
    uart_puts(" mtval=");
    uart_put_hex(pns_save[4]);
    uart_puts(" resume-8=");
    uart_put_hex(pns_save[5] - 8);
    uart_puts(" byte=");
    uart_put_hex(pns_save[1] & 0xFFUL);
    uart_puts("\n");

    if (expect_trap) {
        check(pns_save[6] == 1, "expected trap did not happen");
        check(pns_save[2] == 5, "mcause != 5 (load access fault)");
        check(pns_save[3] == pns_save[5] - 8,
              "mepc != address of faulting lbu");
        check(pns_save[4] == addr, "mtval != faulting address");
    } else {
        check(pns_save[6] == 0, "unexpected trap on allowed access");
        if (check_val)
            check((pns_save[1] & 0xFFUL) == expect_val,
                  "allowed byte read back wrong value");
    }
}

int main(void) {
    unsigned long i, base, pmpaddr_4k, pmpaddr_64k, rb;

    uart_init();
    uart_puts("pmp-napot-size: PMP NAPOT size-decoding test\n");
    uart_puts("hart mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    base = (unsigned long)scratch;
    check((base & 0xFFFFUL) == 0, "scratch start not 64 KiB aligned");
    // base>>2 must have its low 14 bits clear so ORing the size field
    // only sets the trailing-ones run and never disturbs the base.
    check(((base >> 2) & 0x3FFFUL) == 0, "base>>2 low 14 bits not clear");

    // Control 1: the whole 64 KiB window is good RAM before programming.
    for (i = 0; i < sizeof(scratch); i++)
        scratch[i] = (unsigned char)(i & 0xFF);
    for (i = 0; i < sizeof(scratch); i++)
        check(scratch[i] == (unsigned char)(i & 0xFF),
              "scratch not writable before PMP programming");
    // Dedicated sentinel outside the region; also assert it really is
    // outside, since the PMP region is exactly the scratch window.
    far_sentinel = 0xA5;
    check(far_sentinel == 0xA5, "sentinel outside region not writable");
    check((unsigned long)&far_sentinel < base ||
          (unsigned long)&far_sentinel >= base + 0x10000,
          "far_sentinel landed inside the PMP region");
    uart_puts("control: scratch pattern + sentinel writable before PMP: ok\n");

    // Install the trap vector (direct mode) and arm mscratch.
    __asm__ volatile("csrw mtvec, %0" :: "r"(pns_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(pns_save));

    pmpaddr_4k = (base >> 2) | NAPOT_SIZE_4K;
    pmpaddr_64k = (base >> 2) | NAPOT_SIZE_64K;
    uart_puts("config: base=");
    uart_put_hex(base);
    uart_puts(" pmpaddr0=");
    uart_put_hex(pmpaddr_4k);
    uart_puts(" (NAPOT 4 KiB [");
    uart_put_hex(base);
    uart_puts(", ");
    uart_put_hex(base + 0x1000);
    uart_puts(")) pmpaddr1=");
    uart_put_hex(pmpaddr_64k);
    uart_puts(" (NAPOT 64 KiB [");
    uart_put_hex(base);
    uart_puts(", ");
    uart_put_hex(base + 0x10000);
    uart_puts("))\n");

    // ---- Phase A: 4 KiB encoding on entry 0 ----
    uart_puts("phase A: entry0 = locked NAPOT 4 KiB deny\n");
    csr_write_pmpaddr0(pmpaddr_4k);
    csr_write_pmpcfg0(PMP_L_NAPOT_DENY);
    rb = csr_read_pmpaddr0();
    uart_puts("config: pmpaddr0 readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == pmpaddr_4k, "pmpaddr0 readback mismatch");
    rb = csr_read_pmpcfg0();
    uart_puts("config: pmpcfg0 readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == PMP_L_NAPOT_DENY, "pmpcfg0 readback != 0x98");

    // Control 2: an address outside the region stays accessible.
    for (i = 0; i < 4; i++)
        probe_outside[i] = 0x1122334455667788UL + i;
    for (i = 0; i < 4; i++)
        check(probe_outside[i] == 0x1122334455667788UL + i,
              "address outside PMP region became inaccessible");
    uart_puts("control: address outside region still accessible: ok\n");

    // (a) Inside a 64 KiB window but outside the 4 KiB window: no trap.
    test_lbu(base + 0x2000, 0, 0x00UL, 1);
    // (b) Last byte inside the 4 KiB region: traps.
    test_lbu(base + 0x0FFF, 1, 0, 0);
    // (c) First byte outside the 4 KiB region: no trap, pattern byte back.
    test_lbu(base + 0x1000, 0, 0x00UL, 1);

    // ---- Phase B: 64 KiB encoding on entry 1 ----
    uart_puts("phase B: entry1 = locked NAPOT 64 KiB deny\n");
    csr_write_pmpaddr1(pmpaddr_64k);
    // Byte 0 is locked, so this write must leave it at 0x98 while
    // installing byte 1.
    csr_write_pmpcfg0(PMP_L_NAPOT_DENY | (PMP_L_NAPOT_DENY << 8));
    rb = csr_read_pmpaddr1();
    uart_puts("config: pmpaddr1 readback=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == pmpaddr_64k, "pmpaddr1 readback mismatch");
    rb = csr_read_pmpcfg0();
    uart_puts("config: pmpcfg0 readback=");
    uart_put_hex(rb);
    uart_puts(" (locked byte0 must hold 0x98)\n");
    check(rb == (PMP_L_NAPOT_DENY | (PMP_L_NAPOT_DENY << 8)),
          "pmpcfg0 readback != 0x9898");

    // (d) The same address as (a) now traps: the boundary moved with the
    //     encoded size.
    test_lbu(base + 0x2000, 1, 0, 0);
    // (e) Last byte inside the large region: traps.
    test_lbu(base + 0xFFFF, 1, 0, 0);
    // (f) First byte outside the large region: no trap. The value is not
    //     checked (this address aliases the fails counter in this build);
    //     the dedicated sentinel below proves outside readability.
    test_lbu(base + 0x10000, 0, 0, 0);
    // Outside-region readability with a real value: no trap, 0xA5 back.
    test_lbu((unsigned long)&far_sentinel, 0, 0xA5UL, 1);

    if (fails == 0)
        uart_puts("RESULT: PASS (boundary moved 4 KiB -> 64 KiB with the encoding)\n");
    else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
