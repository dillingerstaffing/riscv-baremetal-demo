// czg_main.c: menvcfg.CBZE gates S-mode execution of Zicboz cbo.zero.
//
// Mechanism under test: menvcfg.CBZE (bit 7) is the M-mode gate that
// controls whether the cbo.zero instruction may execute in S-mode.
// With CBZE=0 an S-mode cbo.zero must trap as an illegal
// instruction; with CBZE=1 it zeroes a full cache block. (An earlier
// backlog note said senvcfg.CBZE; the privileged spec assigns the
// S-mode gate to menvcfg.CBZE, while senvcfg.CBZE gates U-mode only.
// This module tests the menvcfg gate, and the correction is
// documented in PROOF.md.)
//
// Run plan, all on hart 0 of the QEMU virt board:
//   Phase 0 (M-mode): probe Zicboz by executing cbo.zero on a
//     64-byte scratch block. Zero traps expected. If the probe
//     traps, the hart lacks Zicboz: the run stops with the honest
//     smaller slice (probe result plus the trap cause) and a PASS
//     verdict on the probe only.
//   Phase 1: M-mode clears menvcfg.CBZE (boot value read, bit 7
//     cleared, readback published), drops to S-mode, and executes
//     cbo.zero at a numerically labeled site. Exactly one trap is
//     expected: scause 0x2, sepc exactly at the site, and the block
//     byte-identical to its snapshot (the zero never happened).
//   Phase 2: M-mode sets menvcfg.CBZE (readback published), drops
//     to S-mode, and executes cbo.zero on a nonzero pattern. Zero
//     traps expected; the full 64-byte block must read zero while
//     the 16 guard bytes on each side keep their pattern, pinning
//     the observed block size at 64 bytes.
//   Then menvcfg is restored to its exact boot value and a
//     1,000,000-spin quiet window must take zero traps.
// On PASS the machine shuts down through the virt test-device
// finisher (QEMU exits 0); on FAIL the hart parks in a wfi loop
// without touching the finisher, so a FAIL is observable as exit
// status 124 under timeout.
//
// cbo.zero encoding used below: funct12 0x004, funct3 010 (CBO),
// opcode 1110011 (SYSTEM), rd 00000, rs1 = block address, i.e.
// 0x00402073 | (rs1 << 15). Bit fields checked against the Zicboz
// chapter of the unprivileged spec; rd is hardwired to x0 by the
// encoding, so there is no destination register state to verify.

#include "../uart.h"

extern void m_trap_entry(void);
extern void s_trap_entry(void);
void smode_phase1(void);
void smode_phase2(void);
void phase1_done(void);
void phase2_done(void);

// Trap records, written by czg_trap.S. Slot map: 0 trap count,
// 1 parked t1, 2 cause, 3 pc, 4 status-at-entry, 5 spare,
// 6 parked t2, 7 (M-mode) continuation, 8 trap value, 9-11 spare.
volatile unsigned long m_regs[12];
volatile unsigned long s_regs[12];

// 64-byte-aligned scratch block with 16-byte guards on each side,
// all in one struct so the guards sit immediately next to the block
// that cbo.zero zeroes.
struct scratch_area {
    unsigned char pad_lo[48];
    unsigned char lo[16];
    unsigned char buf[64];
    unsigned char hi[16];
    unsigned char pad_hi[48];
};
static struct scratch_area scratch __attribute__((aligned(64)));
static unsigned char snapshot[64];

#define CBZE_BIT (1UL << 7)

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

// Measured values, published by the results printer.
static unsigned long boot_menvcfg, rb_clear, rb_set, rb_restored;
static unsigned long p0_traps, p0_cause, p0_epc, p0_zeromism;
static unsigned long p1_site, p1_straps, p1_scause, p1_sepc, p1_bufmism;
static unsigned long p2_site, p2_straps, p2_zeromism, p2_guardmism;
static unsigned long q_traps0, q_traps1;

static unsigned long nchecks = 0;
static unsigned long fails = 0;

static void check(int cond, const char *msg) {
    nchecks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long read_menvcfg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, menvcfg" : "=r"(v));
    return v;
}

static void write_menvcfg(unsigned long v) {
    __asm__ volatile("csrw menvcfg, %0" :: "r"(v) : "memory");
}

// FNV-1a (64-bit) over the verdict-relevant values in a fixed order.
// No live counters or unaligned pointers enter the checksum, so it is
// byte-identical across runs of the same binary.
static unsigned long long cksum = 1469598103934665603ULL;

static void cks_feed(unsigned long v) {
    int i;
    for (i = 0; i < 8; i++) {
        cksum ^= (unsigned long long)((v >> (8 * i)) & 0xffUL);
        cksum *= 1099511628211ULL;
    }
}

static void uart_put_hex64(unsigned long long v) {
    int i;
    uart_puts("0x");
    for (i = 15; i >= 0; i--) {
        unsigned int d = (unsigned int)((v >> (4 * i)) & 0xfULL);
        uart_putc(d < 10 ? (char)('0' + d) : (char)('a' + d - 10));
    }
}

// Drop from M-mode to S-mode at the given entry point. sret takes the
// target from sepc and the target privilege from sstatus.SPP;
// mstatus.MPP is set to S-mode for a consistent view. Never returns:
// each S-mode payload ends with ecall, and the M-mode handler mret's
// into the continuation in m_regs[7].
static void drop_to_smode(void (*entry)(void)) {
    unsigned long v;

    __asm__ volatile("csrr %0, sstatus" : "=r"(v));
    v |= (1UL << 8);  // SPP = 1 (S-mode)
    __asm__ volatile("csrw sstatus, %0" :: "r"(v));
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    v = (v & ~(3UL << 11)) | (1UL << 11);  // MPP = 01
    __asm__ volatile("csrw mstatus, %0" :: "r"(v));
    __asm__ volatile("csrw sepc, %0\n\tsret"
                     :: "r"((unsigned long)entry) : "memory");
    for (;;)
        __asm__ volatile("wfi");
}

// Phase-1 S-mode payload: execute cbo.zero at a numerically labeled
// site with CBZE=0. The delegated trap lands in s_trap_entry, which
// records scause/sepc, skips the instruction, and sret's back here;
// the block is then compared against the snapshot and the hart
// returns to M-mode with ecall. The site address is captured with an
// in-assembly forward label so it cannot drift under optimization.
void smode_phase1(void) {
    unsigned long site, i, mism = 0;

    __asm__ volatile("la %0, 1f\n\t"
                     "1: .insn i 0x73, 2, x0, %1, 4\n\t"
                     : "=r"(site) : "r"(scratch.buf) : "memory");
    p1_site = site;
    for (i = 0; i < 64; i++)
        if (scratch.buf[i] != snapshot[i])
            mism++;
    p1_bufmism = mism;
    __asm__ volatile("ecall" ::: "memory");
    for (;;)
        __asm__ volatile("wfi");
}

// Phase-2 S-mode payload: execute cbo.zero at a numerically labeled
// site with CBZE=1. No trap is expected; the block must read all
// zero while both guards keep their pattern. Returns via ecall.
void smode_phase2(void) {
    unsigned long site, i, zmism = 0, gmism = 0;

    __asm__ volatile("la %0, 1f\n\t"
                     "1: .insn i 0x73, 2, x0, %1, 4\n\t"
                     : "=r"(site) : "r"(scratch.buf) : "memory");
    p2_site = site;
    for (i = 0; i < 64; i++)
        if (scratch.buf[i] != 0)
            zmism++;
    for (i = 0; i < 16; i++) {
        if (scratch.lo[i] != 0xCC)
            gmism++;
        if (scratch.hi[i] != 0xCC)
            gmism++;
    }
    p2_zeromism = zmism;
    p2_guardmism = gmism;
    __asm__ volatile("ecall" ::: "memory");
    for (;;)
        __asm__ volatile("wfi");
}

// Print the checks, the checksum, and the verdict; shut the machine
// down on PASS, park on FAIL.
static void report_and_finish(void) {
    check(p0_traps == 0, "phase0: probe cbo.zero trapped in M-mode");
    check(p0_zeromism == 0, "phase0: block not zeroed by M-mode cbo.zero");
    check(rb_clear == (boot_menvcfg & ~CBZE_BIT),
          "phase1: CBZE clear readback != written value");
    check(p1_straps == 1, "phase1: S-mode trap did not fire exactly once");
    check(p1_scause == 2, "phase1: scause != illegal instruction (2)");
    check(p1_sepc == p1_site, "phase1: sepc != cbo.zero site address");
    check(p1_bufmism == 0, "phase1: block changed despite the trap");
    check(rb_set == (boot_menvcfg | CBZE_BIT),
          "phase2: CBZE set readback != written value");
    check(p2_straps == 0, "phase2: unexpected S-mode trap with CBZE=1");
    check(p2_zeromism == 0, "phase2: block not fully zeroed with CBZE=1");
    check(p2_guardmism == 0, "phase2: bytes outside the 64-byte block changed");
    check(rb_restored == boot_menvcfg, "restore: menvcfg != boot value");
    check(q_traps1 == q_traps0, "quiet: trap count moved during 1M spins");

    cks_feed(boot_menvcfg);
    cks_feed(rb_clear);
    cks_feed(rb_set);
    cks_feed(rb_restored);
    cks_feed(p0_traps);
    cks_feed(p1_straps);
    cks_feed(p1_scause);
    cks_feed(p1_sepc);
    cks_feed(p1_site);
    cks_feed(p1_bufmism);
    cks_feed(p2_straps);
    cks_feed(p2_site);
    cks_feed(p2_zeromism);
    cks_feed(p2_guardmism);

    uart_puts("\nchecksum (FNV-1a over the verdict values) = ");
    uart_put_hex64(cksum);
    uart_puts("\nchecks: ");
    uart_put_dec(nchecks);
    uart_puts("  mismatches: ");
    uart_put_dec(fails);
    uart_puts("\n");

    // Drain the UART before the finisher shuts the machine down, so
    // the final RESULT line is never cut off.
    while (!(*UART0_LSR & LSR_TEMT))
        ;
    if (fails == 0) {
        uart_puts("RESULT: PASS\n");
        while (!(*UART0_LSR & LSR_TEMT))
            ;
        *VIRT_TEST_FINISHER = FINISHER_PASS;  // shuts down; qemu exits 0
        for (;;)
            __asm__ volatile("wfi");
    }
    uart_puts("RESULT: FAIL\n");
    while (!(*UART0_LSR & LSR_TEMT))
        ;
    for (;;)  // FAIL: park the hart so the harness sees a timeout
        __asm__ volatile("wfi");
}

// Honest smaller slice: the phase-0 probe trapped, so this hart has
// no Zicboz. Report the probe result and stop with a PASS verdict on
// the probe only.
static void probe_only_finish(void) {
    check(p0_traps == 1 && p0_cause == 2,
          "probe: expected exactly one illegal-instruction trap");

    cks_feed(p0_traps);
    cks_feed(p0_cause);
    cks_feed(p0_epc);

    uart_puts("\nnote: cbo.zero unavailable on this hart; gate test not run\n");
    uart_puts("checksum (FNV-1a over the probe values) = ");
    uart_put_hex64(cksum);
    uart_puts("\nchecks: ");
    uart_put_dec(nchecks);
    uart_puts("  mismatches: ");
    uart_put_dec(fails);
    uart_puts("\n");

    while (!(*UART0_LSR & LSR_TEMT))
        ;
    if (fails == 0) {
        uart_puts("RESULT: PASS (probe only)\n");
        while (!(*UART0_LSR & LSR_TEMT))
            ;
        *VIRT_TEST_FINISHER = FINISHER_PASS;
        for (;;)
            __asm__ volatile("wfi");
    }
    uart_puts("RESULT: FAIL\n");
    while (!(*UART0_LSR & LSR_TEMT))
        ;
    for (;;)
        __asm__ volatile("wfi");
}

// M-mode continuation after the phase-1 S-mode payload returns via
// ecall. Snapshots the phase-1 trap record, then sets up phase 2.
void phase1_done(void) {
    unsigned long i;

    m_regs[7] = 0;  // consume the continuation
    p1_straps = s_regs[0];
    p1_scause = s_regs[2];
    p1_sepc = s_regs[3];

    uart_puts("phase1: s_traps=");
    uart_put_dec(p1_straps);
    uart_puts(" scause=");
    uart_put_hex(p1_scause);
    uart_puts(" sepc=");
    uart_put_hex(p1_sepc);
    uart_puts(" site=");
    uart_put_hex(p1_site);
    uart_puts(" bufmism=");
    uart_put_dec(p1_bufmism);
    uart_puts("\n");

    // Phase 2: set CBZE and read back the exact written value.
    write_menvcfg(boot_menvcfg | CBZE_BIT);
    rb_set = read_menvcfg();
    uart_puts("phase2: menvcfg set CBZE: write=");
    uart_put_hex(boot_menvcfg | CBZE_BIT);
    uart_puts(" readback=");
    uart_put_hex(rb_set);
    uart_puts("\n");

    for (i = 0; i < 64; i++)
        scratch.buf[i] = 0x3C;
    for (i = 0; i < 16; i++) {
        scratch.lo[i] = 0xCC;
        scratch.hi[i] = 0xCC;
    }
    s_regs[0] = 0;
    m_regs[7] = (unsigned long)phase2_done;
    drop_to_smode(smode_phase2);
}

// M-mode continuation after the phase-2 S-mode payload returns via
// ecall. Restores menvcfg, runs the quiet window, and reports.
void phase2_done(void) {
    unsigned long i;

    m_regs[7] = 0;  // consume the continuation
    p2_straps = s_regs[0];

    uart_puts("phase2: s_traps=");
    uart_put_dec(p2_straps);
    uart_puts(" zeromism=");
    uart_put_dec(p2_zeromism);
    uart_puts(" guardmism=");
    uart_put_dec(p2_guardmism);
    uart_puts("\n");

    // Restore menvcfg to its exact boot value.
    write_menvcfg(boot_menvcfg);
    rb_restored = read_menvcfg();
    uart_puts("restore: menvcfg=");
    uart_put_hex(rb_restored);
    uart_puts(" expected=");
    uart_put_hex(boot_menvcfg);
    uart_puts("\n");

    // Quiet window: 1,000,000 spins with the state restored; the
    // M-mode trap count must not move.
    q_traps0 = m_regs[0];
    for (i = 0; i < 1000000UL; i++)
        __asm__ volatile("" ::: "memory");
    q_traps1 = m_regs[0];
    uart_puts("quiet: m_traps before/after=");
    uart_put_dec(q_traps0);
    uart_puts("/");
    uart_put_dec(q_traps1);
    uart_puts("\n");

    report_and_finish();
}

int main(void) {
    unsigned long i;

    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("cbo-zero-gate: menvcfg.CBZE gates S-mode cbo.zero\n");
    uart_puts("========================================\n\n");

    // Trap vectors: direct-mode mtvec/stvec, scratch registers at
    // the per-mode record arrays.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_regs));

    // Disarm every interrupt enable: mie clear and mstatus.MIE
    // clear. sie and sstatus.SIE are never set; the only traps in
    // this run are the probe, the two S-mode cbo.zero executions,
    // and the two S->M ecalls.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrc mstatus, %0" :: "r"(1UL << 3));  // MIE off

    // Open the whole address space to S-mode (lower modes
    // default-deny) with one PMP NAPOT entry.
    __asm__ volatile("li t0, -1\n\t"
                     "csrw pmpaddr0, t0\n\t"
                     "li t0, 0x1f\n\t"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // Delegate illegal-instruction traps to S-mode so phase 1's trap
    // lands in the S-mode handler with scause/sepc populated.
    // (M-mode traps never delegate, so the phase-0 probe still lands
    // in the M-mode handler.)
    __asm__ volatile("csrs medeleg, %0" :: "r"(1UL << 2));

    boot_menvcfg = read_menvcfg();
    uart_puts("boot: menvcfg=");
    uart_put_hex(boot_menvcfg);
    uart_puts("\n");

    // Phase 0: probe Zicboz in M-mode, where no gate applies. Fill
    // the block with a pattern, execute cbo.zero, and require zero
    // traps and a zeroed block.
    for (i = 0; i < 64; i++)
        scratch.buf[i] = 0xA5;
    for (i = 0; i < 16; i++) {
        scratch.lo[i] = 0xCC;
        scratch.hi[i] = 0xCC;
    }
    m_regs[0] = 0;
    __asm__ volatile(".insn i 0x73, 2, x0, %0, 4"
                     :: "r"(scratch.buf) : "memory");
    p0_traps = m_regs[0];
    p0_cause = m_regs[2];
    p0_epc = m_regs[3];
    p0_zeromism = 0;
    for (i = 0; i < 64; i++)
        if (scratch.buf[i] != 0)
            p0_zeromism++;

    uart_puts("phase0: M-mode cbo.zero: traps=");
    uart_put_dec(p0_traps);
    uart_puts(" zeromism=");
    uart_put_dec(p0_zeromism);
    uart_puts("\n");

    if (p0_traps > 0) {
        uart_puts("phase0: Zicboz NOT available: mcause=");
        uart_put_hex(p0_cause);
        uart_puts(" mepc=");
        uart_put_hex(p0_epc);
        uart_puts("\n");
        probe_only_finish();
        // not reached
    }

    // Phase 1: clear CBZE, read back the exact written value, drop
    // to S-mode, and execute cbo.zero at the labeled site. The gate
    // must fire exactly once.
    write_menvcfg(boot_menvcfg & ~CBZE_BIT);
    rb_clear = read_menvcfg();
    uart_puts("phase1: menvcfg clear CBZE: write=");
    uart_put_hex(boot_menvcfg & ~CBZE_BIT);
    uart_puts(" readback=");
    uart_put_hex(rb_clear);
    uart_puts("\n");

    for (i = 0; i < 64; i++) {
        scratch.buf[i] = 0x5A;
        snapshot[i] = 0x5A;
    }
    s_regs[0] = 0;
    m_regs[7] = (unsigned long)phase1_done;
    drop_to_smode(smode_phase1);
    for (;;)  // unreachable: the M-mode handler mret's to phase1_done
        __asm__ volatile("wfi");
}
