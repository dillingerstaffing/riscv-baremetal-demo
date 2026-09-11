// stw_main.c: sie WARL write/legalized-readback probe of STIE.
//
// Mechanism under test: sie (supervisor interrupt enable, CSR 0x104)
// is a WARL CSR. Every write is accepted, but the readback is the
// legalized value: the subset of enable bits this hart admits. On
// QEMU 8.2.2 the admitted set follows mideleg: an enable bit sticks
// exactly when the matching interrupt is delegated to S-mode. The
// module delegates only the supervisor timer interrupt (mideleg bit
// 5, STI), then shows an all-ones write to sie legalizes to 0x20
// (STIE alone: SSIE and SEIE do not stick because SSI and SEI stay
// M-mode interrupts), and that STIE round-trips through csrs/csrc
// set/clear with the bit reading back as written.
//
// This is the register-legalization sibling of src/sie-stie-gate/,
// which tests interrupt-gating behavior (a pended STIP with STIE
// clear produces no trap; setting STIE produces exactly one). No
// interrupt source is armed or pended here and no trap is expected:
// reaching the completion marker with a park-on-entry trap vector
// installed is the no-trap proof, and the printed trap count is the
// counter the trap entry would have incremented.
//
// Expected values were measured first with a scratch probe, then
// asserted: boot sie=0x0, boot mideleg=0x1444; mideleg write 0x20
// reads back 0x1464; sie all-ones at mideleg=0x1464 reads back
// 0x20; sie zero write reads back 0x0; csrs STIE reads back 0x20;
// csrc STIE reads back 0x0. (Exploratory only: sie all-ones at
// mideleg=0x3666 reads back 0x2222.)
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.

#include "../uart.h"

// sie enable bits: SSIE bit 1, STIE bit 5, SEIE bit 9.
#define SIE_SSIE (1UL << 1)
#define SIE_STIE (1UL << 5)
#define SIE_SEIE (1UL << 9)

// mideleg bit 5 (STI): the only interrupt delegated in this module.
#define MIDELEG_STI (1UL << 5)

// Measured legalized readbacks on QEMU 8.2.2 virt (scratch probe,
// then asserted here).
#define EXPECT_SIE_BOOT     0x0UL
#define EXPECT_MIDELEG_BOOT 0x1444UL
#define EXPECT_MIDELEG_STI  0x1464UL  // 0x1444 forced bits | STI
#define EXPECT_SIE_ONES     0x20UL    // only the delegated STIE sticks

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final RESULT line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

volatile unsigned long stw_trap_count;

extern void stw_trap_entry(void);

static unsigned int checks = 0;
static unsigned int fails = 0;

static void check(int cond, const char *msg) {
    checks++;
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

static unsigned long read_sie(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, sie" : "=r"(v));
    return v;
}

static void write_sie(unsigned long v) {
    __asm__ volatile("csrw sie, %0" :: "r"(v));
}

// Set/clear STIE through a register: bit 5 is not encodable in the
// 5-bit csrsi immediate, so csrs/csrc take the bit in t0.
static void set_stie(void) {
    __asm__ volatile("li t0, 0x20\n csrs sie, t0" ::: "t0");
}

static void clear_stie(void) {
    __asm__ volatile("li t0, 0x20\n csrc sie, t0" ::: "t0");
}

static unsigned long read_mideleg(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mideleg" : "=r"(v));
    return v;
}

static void write_mideleg(unsigned long v) {
    __asm__ volatile("csrw mideleg, %0" :: "r"(v));
}

static unsigned long fnv1a_64(unsigned long h, unsigned long v) {
    h ^= v;
    h *= 0x100000001b3UL;
    return h;
}

int main(void) {
    unsigned long sie_boot, mideleg_boot, mideleg_sti, sie_ones;
    unsigned long sie_zero, sie_csrs, sie_csrc, sie_final;
    unsigned long mideleg_final, csum;

    uart_init();
    uart_puts("sie-stie-write: sie WARL write/legalized-readback probe of STIE\n");

    // 1. Boot baselines.
    sie_boot = read_sie();
    mideleg_boot = read_mideleg();
    uart_puts("boot: sie=");
    uart_put_hex(sie_boot);
    uart_puts(" mideleg=");
    uart_put_hex(mideleg_boot);
    uart_puts("\n");
    check(sie_boot == EXPECT_SIE_BOOT, "sie != 0x0 at boot");
    check(mideleg_boot == EXPECT_MIDELEG_BOOT, "mideleg != 0x1444 at boot");

    // Defensive vector only: no interrupt source is armed and MIE
    // stays clear, so no trap should ever fire. The entry counts
    // the trap and parks; reaching the verdict line is the no-trap
    // proof and the printed count is the entry's own counter.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)stw_trap_entry));
    {
        unsigned long tv;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        check((tv & ~3UL) == (unsigned long)stw_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
    }

    // 2. Delegate only the supervisor timer interrupt, so STIE is
    // the one S-mode enable bit the hart may admit.
    write_mideleg(MIDELEG_STI);
    mideleg_sti = read_mideleg();
    uart_puts("mideleg: write=0x20 readback=");
    uart_put_hex(mideleg_sti);
    uart_puts(" (expect 0x1464)\n");
    check(mideleg_sti == EXPECT_MIDELEG_STI,
          "mideleg STI delegation readback != 0x1464");

    // 3. All-ones write: publish the legalized readback. Only the
    // delegated STIE sticks; SSIE and SEIE stay clear because SSI
    // and SEI remain M-mode interrupts.
    write_sie(~0UL);
    sie_ones = read_sie();
    uart_puts("sie: write=0xffffffffffffffff readback=");
    uart_put_hex(sie_ones);
    uart_puts(" (expect 0x20)\n");
    check(sie_ones == EXPECT_SIE_ONES,
          "sie all-ones legalized readback != 0x20");
    check((sie_ones & SIE_STIE) != 0, "STIE did not stick in sie");
    check((sie_ones & SIE_SSIE) == 0, "SSIE stuck without SSI delegation");
    check((sie_ones & SIE_SEIE) == 0, "SEIE stuck without SEI delegation");

    // 4. Zero write: the legal value returns to 0.
    write_sie(0UL);
    sie_zero = read_sie();
    uart_puts("sie: write=0x0 readback=");
    uart_put_hex(sie_zero);
    uart_puts(" (expect 0x0)\n");
    check(sie_zero == 0, "sie zero-write readback != 0x0");

    // 5. Set STIE through csrs; the bit must read back set and alone.
    set_stie();
    sie_csrs = read_sie();
    uart_puts("sie: csrs STIE readback=");
    uart_put_hex(sie_csrs);
    uart_puts(" (expect 0x20)\n");
    check(sie_csrs == SIE_STIE, "sie readback != 0x20 after csrs STIE");

    // 6. Clear STIE through csrc; the bit must read back clear.
    clear_stie();
    sie_csrc = read_sie();
    uart_puts("sie: csrc STIE readback=");
    uart_put_hex(sie_csrc);
    uart_puts(" (expect 0x0)\n");
    check(sie_csrc == 0, "sie readback != 0x0 after csrc STIE");

    // 7. Restore: sie to 0, mideleg to its boot value.
    write_sie(0UL);
    sie_final = read_sie();
    write_mideleg(EXPECT_MIDELEG_BOOT);
    mideleg_final = read_mideleg();
    uart_puts("restore: sie=");
    uart_put_hex(sie_final);
    uart_puts(" mideleg=");
    uart_put_hex(mideleg_final);
    uart_puts(" (expect 0x0 / 0x1444)\n");
    check(sie_final == 0, "sie not 0x0 after restore");
    check(mideleg_final == EXPECT_MIDELEG_BOOT,
          "mideleg not 0x1444 after restore");

    // No trap may have fired at any point.
    check(stw_trap_count == 0, "trap fired during the run");

    // Checksum over the deterministic measured values only: every
    // input is a register readback or the trap counter, so it is
    // byte-identical across runs.
    csum = 0xcbf29ce484222325UL;
    csum = fnv1a_64(csum, sie_boot);
    csum = fnv1a_64(csum, mideleg_boot);
    csum = fnv1a_64(csum, mideleg_sti);
    csum = fnv1a_64(csum, sie_ones);
    csum = fnv1a_64(csum, sie_zero);
    csum = fnv1a_64(csum, sie_csrs);
    csum = fnv1a_64(csum, sie_csrc);
    csum = fnv1a_64(csum, sie_final);
    csum = fnv1a_64(csum, mideleg_final);
    csum = fnv1a_64(csum, stw_trap_count);

    // Verdict section: deterministic measured values only.
    uart_puts("VERDICT sie_boot=");
    uart_put_hex(sie_boot);
    uart_puts(" mideleg_boot=");
    uart_put_hex(mideleg_boot);
    uart_puts(" mideleg_sti=");
    uart_put_hex(mideleg_sti);
    uart_puts(" sie_ones=");
    uart_put_hex(sie_ones);
    uart_puts(" sie_zero=");
    uart_put_hex(sie_zero);
    uart_puts(" sie_csrs=");
    uart_put_hex(sie_csrs);
    uart_puts(" sie_csrc=");
    uart_put_hex(sie_csrc);
    uart_puts(" sie_final=");
    uart_put_hex(sie_final);
    uart_puts(" mideleg_final=");
    uart_put_hex(mideleg_final);
    uart_puts(" traps=");
    uart_put_dec(stw_trap_count);
    uart_puts(" checksum=");
    uart_put_hex(csum);
    uart_puts("\n");
    uart_puts("checks=");
    uart_put_dec(checks);
    uart_puts(" fails=");
    uart_put_dec(fails);
    uart_puts("\n");

    if (fails == 0)
        uart_puts("RESULT: PASS\n");
    else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }

    // Let the UART drain (TEMT: transmitter fully empty) before
    // touching the finisher device.
    while ((*UART0_LSR & LSR_TEMT) == 0)
        ;

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
