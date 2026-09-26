// low_main.c: PMP NA4 low-edge match-behavior test.
//
// Exactly one mechanism is under test: how a locked NA4 PMP entry
// matches an access address on its LOW edge. pmpaddr0 =
// (CANARY_ADDR >> 2) encodes a 4-byte naturally aligned word at
// CANARY_ADDR (0x80002400): for NA4 the entry covers
// [pmpaddr << 2, (pmpaddr << 2) + 4). pmpcfg0 entry 0 = 0x90 means
// A = NA4 (bits 4:3 = 0b10) with L = 1 and R = W = X = 0: a locked
// deny entry over exactly one 4-byte word.
//
// This is the sibling of src/pmp-na4-match, which proved the
// word interior and the HIGH edge (one byte past the word). The
// edge under test here is the low one: an lbu at CANARY_ADDR - 1
// (the last byte before the denied word) matches no entry and must
// complete under M-mode default-allow, while an lbu at the word's
// first byte traps with the load access fault.
//
// A locked entry is checked against M-mode accesses, so the
// probes run as plain M-mode loads with no MPRV involved. The
// privileged spec grants M-mode default-allow for accesses that
// match no entry when at least one entry is locked, so the
// canary-1 probe's clean completion is a direct measurement of
// that rule, not of an allow entry I placed.
//
// Two lock-imposed facts shape the program, and both are turned
// into checks rather than worked around. First, the lock makes
// pmpcfg0/pmpaddr0 read-only until reset, so the programmed
// values cannot be restored; the end-of-run step attempts the
// restore writes and asserts the readback is still the locked
// values, which is exactly the lock's advertised behavior (the
// pmp-lock-bit module proved the same read-only property with
// all-ones writes). Second, the locked deny entry makes the
// canary word unreadable for the rest of the run, so
// "memory unchanged after the trap" is evidenced by the three
// things that remain observable: the faulting load's destination
// keeps its poison value (a faulting access never commits), the
// neighbor word one byte below the denied word reads back
// bit-identical, and the control readback proved both words
// correct before programming.
//
// Probes (each a single volatile asm block so the layout is exact;
// .option norvc keeps every instruction 4 bytes, so the faulting
// access sits exactly at resume - 4):
//   T1: lbu at CANARY_ADDR - 1 (last byte before the word)
//       completes with 0 traps and returns the top byte of the
//       low neighbor word's known pattern.
//   T2: lbu at CANARY_ADDR (the word's first byte) traps exactly
//       once, mcause = 0x5, mepc = faulting lbu, mtval =
//       CANARY_ADDR, destination keeps its poison value.
//
// A minimal M-mode trap entry (low_trap.S) records mcause/mepc/
// mtval, counts every entry, and resumes at the label the probe
// stored. A quiet window of ordinary M-mode work with the entry
// still programmed must show 0 new traps.

#include "../uart.h"

extern void low_trap_entry(void);

// Trap save area, laid out for low_trap.S:
// [0]=total traps [1]=t1 [2]=mcause [3]=mepc [4]=mtval
// [5]=resume pc [6]=seen flag [7]=unused.
// mscratch points here while a test is armed.
volatile unsigned long low_save[8];

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final Verdict line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

#define CANARY_ADDR 0x80002400UL  // 4-byte-aligned canary word
#define LOW_ADDR    0x800023fcUL  // neighbor word, last byte at canary-1
#define CANARY_WORD 0x41b2c3d4UL  // known pattern; low byte 0xd4
#define LOW_WORD    0x12345678UL  // known pattern; top byte 0x12 at canary-1

#define PMPADDR0_VAL (CANARY_ADDR >> 2)  // 0x20000900: NA4 over the word
#define PMPCFG0_VAL  0x90UL              // entry 0: L=1, A=NA4, R=W=X=0

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
// trapped probe must equal low_save[5] - 4.
static unsigned long probe_load_byte(unsigned long addr) {
    unsigned long got;
    __asm__ volatile(
        ".option norvc\n\t"
        "la t0, 1f\n\t"
        "sd t0, 40(%1)\n\t"    // low_save[5]: resume pc
        "sd zero, 48(%1)\n\t"  // low_save[6]: seen = 0
        "mv t1, %2\n\t"        // poison the destination
        "lbu t1, 0(%3)\n\t"    // probe load: faults iff addr matches
        "1:\n\t"
        "mv %0, t1\n\t"        // capture the destination
        ".option rvc"
        : "=r"(got)
        : "r"(low_save), "r"(POISON64), "r"(addr)
        : "t0", "t1", "memory");
    return got;
}

static unsigned long mem_load32(unsigned long addr) {
    unsigned long v;
    // lwu, not lw: the patterns may have bit 31 set, and a
    // sign-extending lw would fail the equality checks below.
    __asm__ volatile("lwu %0, 0(%1)" : "=r"(v) : "r"(addr));
    return v;
}

// 4-byte canary store: matches the probe word width, so the two
// scratch words never overlap each other.
static void mem_write32(unsigned long addr, unsigned long val) {
    *(volatile unsigned int *)addr = (unsigned int)val;
}

int main(void) {
    unsigned long boot_addr0, boot_cfg0, boot_mstatus;
    unsigned long seen, cause, epc, tval, resume, got, rb, q0, acc;
    int i;
    volatile unsigned long acc_v = 0;

    uart_init();
    uart_puts("pmp-na4-low-edge: PMP locked-NA4 low-edge match test\n");

    // All traps to M-mode, no interrupts: the only traps possible
    // are the probes' own synchronous faults.
    __asm__ volatile("csrw mtvec, %0" :: "r"(low_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"(low_save));
    __asm__ volatile("csrw medeleg, %0" :: "r"(0UL));
    __asm__ volatile("csrw mideleg, %0" :: "r"(0UL));
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrci mstatus, 8");  // MIE off

    boot_addr0 = csr_read_pmpaddr0();
    boot_cfg0 = csr_read_pmpcfg0();
    boot_mstatus = csr_read_mstatus();
    uart_puts("boot: pmpaddr0=");
    uart_put_hex(boot_addr0);
    uart_puts(" pmpcfg0=");
    uart_put_hex(boot_cfg0);
    uart_puts(" mstatus=");
    uart_put_hex(boot_mstatus);
    uart_puts("\n");
    check(boot_addr0 == 0, "pmpaddr0 nonzero at boot");
    check(boot_cfg0 == 0, "pmpcfg0 nonzero at boot");
    fold(boot_addr0);
    fold(boot_cfg0);
    fold(boot_mstatus);

    // Layout guard, two parts. (a) The trap handler's stores land
    // in low_save, which must not be the denied word: low_save
    // lives in .bss, and the check requires the whole 8-word array
    // to end at or below CANARY_ADDR. (b) The 16 KiB boot stack
    // reservation overlaps the scratch page, so the canary word is
    // only safe if the LIVE stack never reaches it. The worst-case
    // stack depth here is main plus one inline probe frame, well
    // under 512 bytes below _stack_top (the napot sibling measured
    // 224 bytes worst case with a deeper call tree), so requiring
    // sp 1 KiB clear of the canary word leaves 2x margin. Both
    // values are run-time constants, identical every run.
    {
        unsigned long sp_now;
        __asm__ volatile("mv %0, sp" : "=r"(sp_now));
        uart_puts("layout: low_save=");
        uart_put_hex((unsigned long)low_save);
        uart_puts(" sp=");
        uart_put_hex(sp_now);
        uart_puts("\n");
        check((unsigned long)low_save + sizeof(low_save) <= CANARY_ADDR,
              "low_save reaches the denied word");
        fold((unsigned long)low_save + sizeof(low_save) <= CANARY_ADDR);
        check(sp_now > CANARY_ADDR + 0x400UL,
              "live stack within 1 KiB of the canary word");
        fold(sp_now > CANARY_ADDR + 0x400UL);
    }

    // Control: both scratch words are good RAM before any PMP
    // programming. Written in M-mode with no entry programmed;
    // 4-byte stores match the probe width so the words never
    // overlap. After programming, the canary word becomes
    // unreadable (locked deny), which is why the readback happens
    // here.
    mem_write32(CANARY_ADDR, CANARY_WORD);
    mem_write32(LOW_ADDR, LOW_WORD);
    rb = mem_load32(CANARY_ADDR);
    check(rb == CANARY_WORD, "canary word not writable before PMP");
    fold(rb);
    rb = mem_load32(LOW_ADDR);
    check(rb == LOW_WORD, "low neighbor word not writable before PMP");
    fold(rb);
    uart_puts("control: scratch words are good RAM: ok\n");

    // Program entry 0: locked NA4 deny over exactly the canary
    // word. pmpaddr0 = canary_addr >> 2 selects NA4's 4-byte
    // window; pmpcfg0 byte 0 = 0x90 sets L=1, A=NA4, R=W=X=0.
    csr_write_pmpaddr0(PMPADDR0_VAL);
    csr_write_pmpcfg0(PMPCFG0_VAL);
    rb = csr_read_pmpaddr0();
    uart_puts("config: pmpaddr0=");
    uart_put_hex(rb);
    uart_puts(" (expect 0x20000900)\n");
    check(rb == PMPADDR0_VAL, "pmpaddr0 readback != 0x20000900");
    fold(rb);
    check(rb == (CANARY_ADDR >> 2), "pmpaddr0 != canary_addr >> 2");
    fold(rb == (CANARY_ADDR >> 2));
    rb = csr_read_pmpcfg0();
    uart_puts("config: pmpcfg0=");
    uart_put_hex(rb);
    uart_puts(" (expect 0x90)\n");
    check(rb == PMPCFG0_VAL, "pmpcfg0 readback != 0x90");
    fold(rb);

    // T1: lbu at canary-1, the last byte before the denied word,
    // matches no entry; M-mode default-allow completes it with no
    // trap and returns the top byte of the low word's pattern.
    got = probe_load_byte(CANARY_ADDR - 1);
    seen = low_save[6];
    cause = low_save[2];
    epc = low_save[3];
    tval = low_save[4];
    resume = low_save[5];
    uart_puts("t1 lbu canary-1 : seen=");
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
    check(seen == 0, "t1: unexpected trap");
    fold(seen);
    check(got == ((LOW_WORD >> 24) & 0xffUL), "t1: wrong byte read back");
    fold(got == ((LOW_WORD >> 24) & 0xffUL));
    fold(got);

    // T2: lbu at the word's first byte traps exactly once.
    got = probe_load_byte(CANARY_ADDR);
    seen = low_save[6];
    cause = low_save[2];
    epc = low_save[3];
    tval = low_save[4];
    resume = low_save[5];
    uart_puts("t2 lbu canary   : seen=");
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
    check(seen == 1, "t2: trap count != 1");
    fold(seen);
    check(cause == 0x5UL, "t2: mcause != 0x5 (load access fault)");
    check(epc == resume - 4, "t2: mepc != faulting lbu");
    check(tval == CANARY_ADDR, "t2: mtval != canary address");
    check(got == POISON64, "t2: faulting load overwrote its destination");
    fold(cause);
    fold(epc == resume - 4);
    fold(tval == CANARY_ADDR);
    fold(got == POISON64);

    // The locked entry makes the canary word unreadable for the
    // rest of the run, so its intactness is evidenced by what
    // remains observable: the faulting load never committed
    // (destination kept its poison, checked above), and the
    // neighbor word one byte below the denied word reads back
    // bit-identical, proving the trap had no side effect on the
    // adjacent word.
    rb = mem_load32(LOW_ADDR);
    uart_puts("t2 low neighbor : word=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == LOW_WORD, "t2: low neighbor word changed by the trap");
    fold(rb == LOW_WORD);

    // Quiet window: ordinary M-mode work with the entry still
    // programmed and no probe armed. Any trap here would be
    // unexpected; the handler counts every entry in low_save[0].
    q0 = low_save[0];
    for (i = 0; i < 10000; i++) {
        unsigned long ms;
        __asm__ volatile("csrr %0, mstatus" : "=r"(ms));
        acc_v ^= ms ^ (unsigned long)i;
    }
    acc = acc_v;
    uart_puts("quiet: traps before=");
    uart_put_dec(q0);
    uart_puts(" after=");
    uart_put_dec(low_save[0]);
    uart_puts("\n");
    check(low_save[0] == q0, "quiet window saw an unexpected trap");
    fold(low_save[0] - q0);
    fold(acc);  // deterministic: mstatus and the loop index only

    // Restore attempt. The L bit makes pmpcfg0 and pmpaddr0
    // read-only until reset, so the boot-value writes are legally
    // ignored; the check asserts the programmed locked values
    // persist bit-for-bit, which is exactly the lock's advertised
    // behavior. mstatus restores normally.
    csr_write_pmpaddr0(boot_addr0);
    csr_write_pmpcfg0(boot_cfg0);
    rb = csr_read_pmpaddr0();
    uart_puts("lock: pmpaddr0 after restore attempt=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == PMPADDR0_VAL, "locked pmpaddr0 did not persist");
    fold(rb);
    rb = csr_read_pmpcfg0();
    uart_puts("lock: pmpcfg0 after restore attempt=");
    uart_put_hex(rb);
    uart_puts("\n");
    check(rb == PMPCFG0_VAL, "locked pmpcfg0 did not persist");
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
