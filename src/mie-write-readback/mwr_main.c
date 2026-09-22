// mwr_main.c: mie WARL write/readback probe (all-ones write).
//
// Mechanism under test: the legalization behavior of the mie CSR on
// a write of all ones. mie is the plain read/write M-mode interrupt
// enable register: one enable bit per interrupt cause (bits 11 MEIE,
// 9 SEIE, 7 MTIE, 5 STIE, 3 MSIE, 1 SSIE on the standard layout).
// Writing an enable bit cannot by itself take an interrupt: delivery
// additionally requires mstatus.MIE set, and the run keeps MIE clear
// from boot to end, so asserting every enable bit at once is safe.
// The run measures exactly which bits survive an all-ones write:
//
//   Phase 1: write all-ones to mie, read back and publish the
//   readback (which enable bits the implementation provides). Then
//   write zero and read back; it must read exactly 0.
//   Phase 2: per-bit probe of bits 0..11: for each bit b write
//   (1UL << b) and read back. The readback must be exactly
//   (1UL << b) (bit sticks) or exactly 0 (bit not provided), with
//   no other bits set.
//   Phase 3: write all-ones again. The readback must equal the
//   phase-1 readback (consistency), and its low 12 bits must equal
//   the OR of the individual per-bit results (the per-bit probe
//   covers bits 0..11 only; bits above 11 are observed in the
//   published readback but not individually probed).
//   Phase 4: restore mie to the boot value and verify it reads back
//   byte-identical; verify the trap count is 0 for the whole run.
//
// The trap handler is installed anyway (direct-mode mtvec, mscratch
// scratch area); it records mcause/mepc/mtval and counts entries,
// and the verdict requires that count to be 0, so any unexpected
// trap becomes visible instead of silent. No interrupt can be taken
// because mstatus.MIE stays clear the entire run, verified at boot
// and again at the end.
//
// Every check increments the checks counter; a failed check prints
// FAIL and increments the mismatches counter. RESULT: PASS is
// printed only when every check held. The verdict-relevant values
// (boot baselines, every write/readback pair, the trap count) feed
// a 64-bit FNV-1a digest printed as the last data line, so the
// three bench runs can be compared for byte-identical output.
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.
//
// CSR accesses use direct inline asm. No labels-as-values anywhere,
// so the GCC label-miscompile concern does not arise; the trap
// handler is a plain .S entry point.

#include "../uart.h"

#define MSTATUS_MIE   (1UL << 3)
#define ALL_ONES      (~0UL)

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

extern void mwr_trap_entry(void);

// Trap scratch: slot 0 trap count, slot 1 mcause, slot 2 mepc,
// slot 3 mtval, slot 4 parked t1. BSS-cleared to zero by boot.S.
unsigned long mwr_regs[5];

static unsigned long read_mie(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mie" : "=r"(v));
    return v;
}

static unsigned long read_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static unsigned long read_mideleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mideleg" : "=r"(v));
    return v;
}

static void write_mie(unsigned long v) {
    __asm__ volatile("csrw mie, %0" : : "r"(v));
}

// FNV-1a 64-bit over the verdict-relevant values.
static unsigned long long fnv = 14695981039346656037ULL;
static void digest64(unsigned long v) {
    for (int i = 0; i < 8; i++) {
        fnv ^= (unsigned long long)((v >> (i * 8)) & 0xffUL);
        fnv *= 1099511628211ULL;
    }
}

static int checks = 0;
static int mismatches = 0;

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        mismatches++;
    }
}

int main(void) {
    unsigned long mie, mstatus, mtvec;
    unsigned long boot_mie, boot_mstatus, boot_mideleg;
    unsigned long r1, rb;
    unsigned long sticky_union;
    int b;

    uart_init();
    uart_puts("mie-write-readback: mie all-ones WARL write/readback probe\n");

    // Global interrupt enable must be clear at boot: without MIE no
    // trap can be taken no matter what mie says.
    mstatus = read_mstatus();
    check((mstatus & MSTATUS_MIE) == 0, "mstatus.MIE set at boot");

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the recording area.
    __asm__ volatile("csrw mtvec, %0" : : "r"((unsigned long)mwr_trap_entry));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mwr_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    uart_puts("trap: mtvec=");
    uart_put_hex(mtvec);
    uart_puts("\n");
    check((mtvec & ~3UL) == (unsigned long)mwr_trap_entry,
          "mtvec did not take the handler address");
    check((mtvec & 3UL) == 0, "mtvec not in direct mode");

    // Baselines at boot.
    boot_mie = read_mie();
    boot_mstatus = read_mstatus();
    boot_mideleg = read_mideleg();
    uart_puts("boot: mie=");
    uart_put_hex(boot_mie);
    uart_puts(" mstatus=");
    uart_put_hex(boot_mstatus);
    uart_puts(" mideleg=");
    uart_put_hex(boot_mideleg);
    uart_puts("\n");
    digest64(boot_mie);
    digest64(boot_mstatus);
    digest64(boot_mideleg);
    check((boot_mstatus & MSTATUS_MIE) == 0, "mstatus.MIE not clear at boot");

    // Phase 1: all-ones write publishes which enable bits the
    // implementation provides. A zero write must read back exactly
    // zero: every bit the implementation provides is writable.
    write_mie(ALL_ONES);
    r1 = read_mie();
    uart_puts("phase1 ones: mie=");
    uart_put_hex(r1);
    uart_puts("\n");
    digest64(ALL_ONES);
    digest64(r1);

    write_mie(0);
    rb = read_mie();
    uart_puts("phase1 zero: mie=");
    uart_put_hex(rb);
    uart_puts("\n");
    digest64(rb);
    check(rb == 0, "phase 1: zero write did not read back 0");

    // Phase 2: per-bit probe of bits 0..11. Each write must read
    // back exactly the written bit (sticks) or exactly 0 (not
    // provided), with no other bits set either way.
    sticky_union = 0;
    for (b = 0; b < 12; b++) {
        write_mie(1UL << b);
        rb = read_mie();
        uart_puts("  bit ");
        uart_put_dec((unsigned long)b);
        uart_puts(": mie=");
        uart_put_hex(rb);
        uart_puts("\n");
        digest64(1UL << b);
        digest64(rb);
        if (rb == (1UL << b)) {
            sticky_union |= rb;
        } else if (rb != 0) {
            check(0, "phase 2: per-bit write read back stray bits");
        }
    }
    check(sticky_union != 0 || r1 == 0,
          "phase 2: no bit stuck but phase 1 read back nonzero");
    uart_puts("phase2 sticky-union: mie=");
    uart_put_hex(sticky_union);
    uart_puts("\n");
    digest64(sticky_union);

    // Phase 3: all-ones again. The readback must equal the phase-1
    // readback (consistency). The per-bit probe covers only bits
    // 0..11 (the standard interrupt-enable positions), so the
    // structural cross-check is scoped to the low 12 bits: the bits
    // that stick one at a time are exactly the low bits that stick
    // all at once. Bits above 11 are observed in the published
    // readback but not individually probed (out of scope).
    write_mie(ALL_ONES);
    rb = read_mie();
    uart_puts("phase3 ones: mie=");
    uart_put_hex(rb);
    uart_puts("\n");
    digest64(rb);
    check(rb == r1, "phase 3: all-ones readback differs from phase 1");
    check((rb & 0xfffUL) == sticky_union,
          "phase 3: low-12-bit all-ones readback differs from per-bit union");

    // Phase 4: restore mie to the exact boot value and verify the
    // readback is byte-identical. MIE must still be clear.
    write_mie(boot_mie);
    mie = read_mie();
    mstatus = read_mstatus();
    uart_puts("restored: mie=");
    uart_put_hex(mie);
    uart_puts(" mstatus=");
    uart_put_hex(mstatus);
    uart_puts("\n");
    digest64(mie);
    digest64(mstatus);
    check(mie == boot_mie, "mie did not restore to the boot value");
    check((mstatus & MSTATUS_MIE) == 0, "mstatus.MIE changed during run");

    // The trap handler must never have fired.
    digest64(mwr_regs[0]);
    if (mwr_regs[0] != 0) {
        uart_puts("  FAIL: unexpected trap(s), count=");
        uart_put_dec(mwr_regs[0]);
        uart_puts(" mcause=");
        uart_put_hex(mwr_regs[1]);
        uart_puts(" mepc=");
        uart_put_hex(mwr_regs[2]);
        uart_puts(" mtval=");
        uart_put_hex(mwr_regs[3]);
        uart_puts("\n");
        checks++;
        mismatches++;
    }
    uart_puts("traps: count=");
    uart_put_dec(mwr_regs[0]);
    uart_puts("\n");

    uart_puts("checks: ");
    uart_put_dec((unsigned long)checks);
    uart_puts(" mismatches: ");
    uart_put_dec((unsigned long)mismatches);
    uart_puts("\n");
    uart_puts("digest: ");
    uart_put_hex(fnv);
    uart_puts("\n");

    if (mismatches == 0) {
        uart_puts("RESULT: PASS\n");
        *VIRT_TEST_FINISHER = FINISHER_PASS;
        for (;;) { }
    }
    uart_puts("RESULT: FAIL\n");
    // Park the hart; the harness observes the timeout exit status.
    for (;;) {
        __asm__ volatile("wfi");
    }
}
