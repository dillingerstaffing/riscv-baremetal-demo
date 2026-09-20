// tor_top_main.c: PMP TOR exclusive-top-bound test.
//
// Exactly one mechanism is under test: a TOR PMP entry matches the
// half-open range [pmpaddr[i-1], pmpaddr[i]), so an access at exactly
// the top bound does NOT match the entry. The probes are: an lbu at
// the last byte inside the denied range traps with the load access
// fault; an lbu at exactly the top bound completes with no trap and
// returns the expected byte.
//
// Two entries are programmed. Entry 0 is TOR over [0, base) with
// full permissions, unlocked; entry 1 is TOR over [base, base + 4 KiB)
// with no permissions and the lock bit set, i.e. pmpcfg0 = 0x880F
// (entry 0: TOR + R|W|X; entry 1: L + TOR, R = W = X = 0). A locked
// TOR deny cannot be entry 0: entry 0's TOR range bottom is fixed at
// address 0, so a locked deny there would deny the program's own
// code, the UART, and the test finisher, and no probe could run.
// The top bound under test belongs to entry 1, the locked entry.
//
// A locked entry is checked against M-mode accesses, so the probes
// run as plain M-mode loads with no MPRV involved. The privileged
// spec grants M-mode default-allow for accesses that match no entry
// when at least one entry is locked, so the top-bound probe's clean
// completion is a direct measurement of that rule, not of an allow
// entry: entry 0's unlocked byte is cleared (csrw pmpcfg0, 0) right
// after programming, which also verifies the per-entry lock rule
// (entry 1's byte stays 0x88, entry 0's clears to 0x00), and every
// probe runs with only the locked entry programmed.
//
// Two lock-imposed facts shape the program, and both are turned
// into checks rather than worked around. First, the lock makes
// pmpaddr1 and pmpcfg0's entry-1 byte read-only until reset (and,
// for TOR, it pins pmpaddr0, entry 1's bottom bound), so the
// programmed values cannot be restored; the end-of-run step
// attempts the restore writes and asserts the readbacks are still
// the programmed values bit-for-bit. Second, the locked deny entry
// makes the 4 KiB scratch region unreadable for the rest of the
// run, so "memory unchanged after the trap" is evidenced by the
// things that remain observable: the faulting load's destination
// keeps its poison value (a faulting access never commits), and
// the sentinel byte at exactly the top bound (outside the denied
// range) reads back bit-identical, proving the trap had no side
// effect on the adjacent byte.
//
// Probes (each a single volatile asm block so the layout is exact;
// .option norvc keeps every instruction 4 bytes, so the faulting
// access sits exactly at resume - 4):
//   T1: lbu at base + 4 KiB - 1 (last byte inside the denied range)
//       traps exactly once, mcause = 0x5, mepc = faulting lbu,
//       mtval = base + 4 KiB - 1, destination keeps its poison.
//   T2: lbu at base + 4 KiB (the exclusive top bound) matches no
//       entry, completes with 0 traps, and returns the sentinel.
//
// A minimal M-mode trap entry (tor_top_trap.S) records
// mcause/mepc/mtval, counts every entry, and resumes at the label
// the probe stored. A quiet window of ordinary M-mode work with the
// entry still programmed must show 0 new traps.

#include "../uart.h"

extern void tor_top_trap_entry(void);

// Trap save area, laid out for tor_top_trap.S:
// [0]=total traps [1]=t1 [2]=mcause [3]=mepc [4]=mtval
// [5]=resume pc [6]=seen flag [7]=unused.
// mscratch points here while a test is armed.
volatile unsigned long tor_top_save[8];

// 4 KiB denied region plus one owned byte. region[0..4095] is the
// denied range [start, end); region[4096] sits at exactly the
// exclusive top bound `end` and holds the sentinel. Owning that byte
// explicitly matters: zero-initialized globals (checks, fails) live
// in .bss and the linker places them right after a 4096-byte array,
// so a bare `end` address would alias one of them.
static volatile unsigned char region[4097] __attribute__((aligned(4096)));
#define REGION_LEN 4096UL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final Verdict line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

#define SENTINEL 0xa5UL  // byte stamped at the exclusive top bound

#define PMPCFG0_LAYOUT 0x880FUL  // entry 0: TOR+R|W|X; entry 1: L+TOR deny
#define PMPCFG0_LOCKED 0x8800UL  // entry 1's byte after the clear attempt

#define POISON64 0xdeadbeefdeadbeefUL

static unsigned long checks = 0;
static unsigned long fails = 0;
static unsigned long csum = 1469598103934665603UL;  // FNV-1a 64 offset basis

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

// Fold one 64-bit value into the FNV-1a checksum.
static void fold(unsigned long v) {
    int i;
    for (i = 0; i < 8; i++) {
        csum ^= (v >> (i * 8)) & 0xffUL;
        csum *= 1099511628211UL;
    }
}

static unsigned long csr_read_pmpaddr0(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpaddr0" : "=r"(v));
    return v;
}

static void csr_write_pmpaddr0(unsigned long v) {
    __asm__ volatile("csrw pmpaddr0, %0" :: "r"(v));
}

static unsigned long csr_read_pmpaddr1(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpaddr1" : "=r"(v));
    return v;
}

static void csr_write_pmpaddr1(unsigned long v) {
    __asm__ volatile("csrw pmpaddr1, %0" :: "r"(v));
}

static unsigned long csr_read_pmpcfg0(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(v));
    return v;
}

static void csr_write_pmpcfg0(unsigned long v) {
    __asm__ volatile("csrw pmpcfg0, %0" :: "r"(v));
}

static unsigned long csr_read_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

// Probe byte load in plain M-mode. Returns the loaded byte, or
// POISON64 if the load trapped (a faulting load never writes its
// destination). The trap handler resumes at label 1; the faulting
// lbu is the 4-byte instruction immediately before it, so mepc of a
// trapped probe must equal tor_top_save[5] - 4.
static unsigned long probe_load_byte(unsigned long addr) {
    unsigned long got;
    __asm__ volatile(
        ".option norvc\n\t"
        "la t0, 1f\n\t"
        "sd t0, 40(%1)\n\t"    // tor_top_save[5]: resume pc
        "sd zero, 48(%1)\n\t"  // tor_top_save[6]: seen = 0
        "mv t1, %2\n\t"        // poison the destination
        "lbu t1, 0(%3)\n\t"    // probe load: faults iff addr matches
        "1:\n\t"
        "mv %0, t1\n\t"        // capture the destination
        ".option rvc"
        : "=r"(got)
        : "r"(tor_top_save), "r"(POISON64), "r"(addr)
        : "t0", "t1", "memory");
    return got;
}

static void mem_write8(unsigned long addr, unsigned char val) {
    *(volatile unsigned char *)addr = val;
}

static unsigned char mem_read8(unsigned long addr) {
    return *(volatile unsigned char *)addr;
}

int main(void) {
    unsigned long boot_addr0, boot_addr1, boot_cfg0, boot_mstatus;
    unsigned long start, end, pmpaddr0, pmpaddr1, sp_now;
    unsigned long seen, cause, epc, tval, resume, got, rb, q0, acc;
    int i;
    volatile unsigned long acc_v = 0;

    uart_init();
    uart_puts("pmp-tor-top: PMP TOR exclusive-top-bound test\n");

    // All traps to M-mode, no interrupts: the only traps possible
    // are the probes' own synchronous faults.
    __asm__ volatile("csrw mtvec, %0" :: "r"(tor_top_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(tor_top_save));
    __asm__ volatile("csrw medeleg, %0" :: "r"(0UL));
    __asm__ volatile("csrw mideleg, %0" :: "r"(0UL));
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrci mstatus, 8");  // MIE off

    boot_addr0 = csr_read_pmpaddr0();
    boot_addr1 = csr_read_pmpaddr1();
    boot_cfg0 = csr_read_pmpcfg0();
    boot_mstatus = csr_read_mstatus();
    uart_puts("boot: pmpaddr0=");
    uart_put_hex(boot_addr0);
    uart_puts(" pmpaddr1=");
    uart_put_hex(boot_addr1);
    uart_puts(" pmpcfg0=");
    uart_put_hex(boot_cfg0);
    uart_puts(" mstatus=");
    uart_put_hex(boot_mstatus);
    uart_puts("\n");
    check(boot_addr0 == 0, "pmpaddr0 nonzero at boot");
    check(boot_addr1 == 0, "pmpaddr1 nonzero at boot");
    check(boot_cfg0 == 0, "pmpcfg0 nonzero at boot");
    fold(boot_addr0);
    fold(boot_addr1);
    fold(boot_cfg0);
    fold(boot_mstatus);

    start = (unsigned long)region;
    end = start + REGION_LEN;  // == &region[4096], the owned sentinel byte

    // Layout guard, four parts. (a) The denied region must start on
    // a 4 KiB boundary so pmpaddr1 = end >> 2 is exact. (b) The trap
    // handler's stores land in tor_top_save, which must not be the
    // denied region: tor_top_save lives in .bss before the
    // 4 KiB-aligned region, and the check requires the whole 8-word
    // array to end at or below the region base. (c) The check
    // counters themselves (zero-initialized, hence .bss) must not
    // alias the probed range or the sentinel byte: the linker is
    // free to place them right after the region array. (d) The
    // sentinel byte sits at exactly the top bound; the live stack
    // must stay 1 KiB clear of it so nothing clobbers the expected
    // value. All values are run-time constants, identical every run.
    {
        __asm__ volatile("mv %0, sp" : "=r"(sp_now));
        uart_puts("layout: start=");
        uart_put_hex(start);
        uart_puts(" end=");
        uart_put_hex(end);
        uart_puts(" tor_top_save=");
        uart_put_hex((unsigned long)tor_top_save);
        uart_puts(" sp=");
        uart_put_hex(sp_now);
        uart_puts("\n");
        check((start & 0xFFFUL) == 0, "region start not 4 KiB aligned");
        fold((start & 0xFFFUL) == 0);
        check((unsigned long)tor_top_save + sizeof(tor_top_save) <= start,
              "tor_top_save reaches the denied region");
        fold((unsigned long)tor_top_save + sizeof(tor_top_save) <= start);
        check((unsigned long)&checks < start || (unsigned long)&checks > end,
              "checks counter aliases the probed range");
        fold((unsigned long)&checks < start || (unsigned long)&checks > end);
        check((unsigned long)&fails < start || (unsigned long)&fails > end,
              "fails counter aliases the probed range");
        fold((unsigned long)&fails < start || (unsigned long)&fails > end);
        check(sp_now > end + 0x400UL,
              "live stack within 1 KiB of the top-bound sentinel");
        fold(sp_now > end + 0x400UL);
    }

    // Control: the whole 4 KiB region is good RAM before any PMP
    // programming, so the later T1 fault can only come from the PMP
    // check and not from a bad address anywhere in the range.
    for (i = 0; i < 4096; i++)
        region[i] = (unsigned char)(i & 0xFF);
    for (i = 0; i < 4096; i++)
        check(region[i] == (unsigned char)(i & 0xFF),
              "region not writable before PMP programming");
    fold(region[0]);
    fold(region[2048]);
    fold(region[4095]);
    uart_puts("control: 4 KiB region readable/writable before PMP: ok\n");

    // Stamp the sentinel at exactly the top bound, outside the
    // region that will be denied, and read it back before
    // programming: T2 must verify a real value, not merely the
    // absence of a trap.
    mem_write8(end, (unsigned char)SENTINEL);
    rb = mem_read8(end);
    check(rb == SENTINEL, "top-bound sentinel not writable before PMP");
    fold(rb);
    uart_puts("control: sentinel 0xa5 at the top bound: ok\n");

    // Program the entries. Entry 0: TOR over [0, start) with full
    // permissions, unlocked. Entry 1: TOR over [start, end) with no
    // permissions, locked. pmpaddr0 = start >> 2 is entry 1's
    // bottom bound; pmpaddr1 = end >> 2 is the top bound under test.
    pmpaddr0 = start >> 2;
    pmpaddr1 = end >> 2;
    csr_write_pmpaddr0(pmpaddr0);
    csr_write_pmpaddr1(pmpaddr1);
    csr_write_pmpcfg0(PMPCFG0_LAYOUT);
    rb = csr_read_pmpaddr0();
    uart_puts("config: pmpaddr0=");
    uart_put_hex(rb);
    uart_puts(" (expect start>>2)\n");
    check(rb == pmpaddr0, "pmpaddr0 readback != start >> 2");
    fold(rb);
    rb = csr_read_pmpaddr1();
    uart_puts("config: pmpaddr1=");
    uart_put_hex(rb);
    uart_puts(" (expect end>>2)\n");
    check(rb == pmpaddr1, "pmpaddr1 readback != end >> 2");
    fold(rb);
    rb = csr_read_pmpcfg0();
    uart_puts("config: pmpcfg0=");
    uart_put_hex(rb);
    uart_puts(" (expect 0x880f)\n");
    check(rb == PMPCFG0_LAYOUT, "pmpcfg0 readback != 0x880f");
    fold(rb);

    // The lock on entry 1 must hold per entry: clearing pmpcfg0
    // leaves entry 1's byte at 0x88 while entry 0's unlocked byte
    // clears to 0x00. From here on only the locked entry is
    // programmed, so the top-bound probe's clean completion is a
    // direct measurement of M-mode default-allow.
    csr_write_pmpcfg0(0);
    rb = csr_read_pmpcfg0();
    uart_puts("config: pmpcfg0 after clear attempt=");
    uart_put_hex(rb);
    uart_puts(" (entry1 byte 0x88 locked, entry0 byte cleared)\n");
    check((rb & 0xFF00UL) == (PMPCFG0_LOCKED & 0xFF00UL),
          "locked entry1 pmpcfg byte was modified");
    fold((rb & 0xFF00UL) == (PMPCFG0_LOCKED & 0xFF00UL));
    check((rb & 0xFFUL) == 0,
          "unlocked entry0 pmpcfg byte did not clear");
    fold((rb & 0xFFUL) == 0);

    // T1: lbu at the last byte inside the denied range traps
    // exactly once.
    got = probe_load_byte(end - 1);
    seen = tor_top_save[6];
    cause = tor_top_save[2];
    epc = tor_top_save[3];
    tval = tor_top_save[4];
    resume = tor_top_save[5];
    uart_puts("t1 lbu end-1    : seen=");
    uart_put_dec(seen);
    uart_puts(" mcause=");
    uart_put_hex(cause);
    uart_puts(" mepc=");
    uart_put_hex(epc);
    uart_puts(" mtval=");
    uart_put_hex(tval);
    uart_puts(" resume-4=");
    uart_put_hex(resume - 4);
    uart_puts(" val=");
    uart_put_hex(got);
    uart_puts("\n");
    check(seen == 1, "t1: trap count != 1");
    fold(seen);
    check(cause == 0x5UL, "t1: mcause != 0x5 (load access fault)");
    check(epc == resume - 4, "t1: mepc != faulting lbu");
    check(tval == end - 1, "t1: mtval != last byte inside the range");
    check(got == POISON64, "t1: faulting load overwrote its destination");
    fold(cause);
    fold(epc == resume - 4);
    fold(tval == end - 1);
    fold(got == POISON64);

    // The locked entry makes the region unreadable for the rest of
    // the run, so the trap's lack of side effects is evidenced by
    // what remains observable: the faulting load never committed
    // (destination kept its poison, checked above), and the
    // sentinel byte at the top bound, just outside the denied
    // range, reads back bit-identical.
    rb = mem_read8(end);
    uart_puts("t1 neighbor     : sentinel=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == SENTINEL, "t1: top-bound sentinel changed by the trap");
    fold(rb == SENTINEL);

    // T2: lbu at exactly the top bound. The TOR range is half-open,
    // so no entry matches; with a locked entry programmed, M-mode
    // default-allow completes the access with 0 traps and returns
    // the sentinel.
    got = probe_load_byte(end);
    seen = tor_top_save[6];
    cause = tor_top_save[2];
    epc = tor_top_save[3];
    tval = tor_top_save[4];
    resume = tor_top_save[5];
    uart_puts("t2 lbu end      : seen=");
    uart_put_dec(seen);
    uart_puts(" mcause=");
    uart_put_hex(cause);
    uart_puts(" mepc=");
    uart_put_hex(epc);
    uart_puts(" mtval=");
    uart_put_hex(tval);
    uart_puts(" resume-4=");
    uart_put_hex(resume - 4);
    uart_puts(" val=");
    uart_put_hex(got);
    uart_puts("\n");
    check(seen == 0, "t2: unexpected trap at the exclusive top bound");
    fold(seen);
    check(got == SENTINEL, "t2: wrong byte read back");
    fold(got == SENTINEL);
    fold(got);

    // Quiet window: ordinary M-mode work with the entry still
    // programmed and no probe armed. Any trap here would be
    // unexpected; the handler counts every entry in
    // tor_top_save[0].
    q0 = tor_top_save[0];
    for (i = 0; i < 10000; i++) {
        unsigned long ms;
        __asm__ volatile("csrr %0, mstatus" : "=r"(ms));
        acc_v ^= ms ^ (unsigned long)i;
    }
    acc = acc_v;
    uart_puts("quiet: traps before=");
    uart_put_dec(q0);
    uart_puts(" after=");
    uart_put_dec(tor_top_save[0]);
    uart_puts("\n");
    check(tor_top_save[0] == q0, "quiet window saw an unexpected trap");
    fold(tor_top_save[0] - q0);
    fold(acc);  // deterministic: mstatus and the loop index only

    // Restore attempt. The L bit makes pmpaddr1 and pmpcfg0's
    // entry-1 byte read-only until reset, and for TOR it also pins
    // pmpaddr0 (entry 1's bottom bound), so all three writes are
    // legally ignored; the checks assert the programmed values
    // persist bit-for-bit, which is exactly the lock's advertised
    // behavior. Entry 0's unlocked byte was already cleared above,
    // and mstatus restores normally.
    csr_write_pmpaddr0(0);
    csr_write_pmpaddr1(0);
    csr_write_pmpcfg0(0);
    rb = csr_read_pmpaddr0();
    uart_puts("lock: pmpaddr0 after restore attempt=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == pmpaddr0, "pinned pmpaddr0 did not persist");
    fold(rb);
    rb = csr_read_pmpaddr1();
    uart_puts("lock: pmpaddr1 after restore attempt=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == pmpaddr1, "locked pmpaddr1 did not persist");
    fold(rb);
    rb = csr_read_pmpcfg0();
    uart_puts("lock: pmpcfg0 after restore attempt=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == PMPCFG0_LOCKED, "locked pmpcfg0 did not persist");
    fold(rb);
    __asm__ volatile("csrw mstatus, %0" :: "r"(boot_mstatus));
    check(csr_read_mstatus() == boot_mstatus, "mstatus not restored");
    fold(csr_read_mstatus() == boot_mstatus);

    uart_puts("Checks: ");
    uart_put_dec(checks);
    uart_puts("\nMismatches: ");
    uart_put_dec(fails);
    uart_puts("\nChecksum: ");
    uart_put_hex(csum);
    uart_puts("\nEnvironment: QEMU 8.2.2\n");
    if (fails == 0)
        uart_puts("Verdict: PASS\n");
    else {
        uart_puts("Verdict: FAIL (");
        uart_put_dec(fails);
        uart_puts(" checks failed)\n");
    }

    while (!(*UART0_LSR & LSR_TEMT))
        ;
    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    // FAIL: park the hart without touching the finisher device.
    for (;;)
        __asm__ volatile("wfi");
}
