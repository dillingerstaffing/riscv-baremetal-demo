// mmsw_main.c: mip.MSIP software-write probe in M-mode.
//
// Verified finding on QEMU 8.2.2: mip bit 3 (MSIP) is NOT
// software-writable through the CSR. Writes via csrsi/csrci/csrs
// are ignored (WARL): the readback is unchanged and the bit never
// sets. The bit is driven by the CLINT msip MMIO register instead,
// which is the mechanism src/mip-pending-no-trap/ exercises. The
// two modules test different mechanisms and share no behavior.
//
// The module proves the negative with a control: the same
// immediate-form CSR write sets mip bit 1 (SSIP) and reads back,
// so the write path itself works and the ignored write is
// specific to MSIP. Throughout the run mie.MSIE (bit 3 of mie)
// and mstatus.MIE stay clear, so the pending machine timer
// interrupt (mip bit 7, asserted at boot because mtimecmp reads 0
// at reset) also stays undelivered: the trap count is 0 across a
// bounded spin window.
//
// Boot baselines measured on QEMU 8.2.2 virt: mip=0x80 (MTIP
// pends), mie=0x0, mstatus=0xa00000000 (the read-only UXL/SXL
// fields reporting 64-bit).
//
// Exit/fail code: on PASS the module writes the virt test-device
// finisher word 0x5555 at 0x100000, which shuts the machine down
// and QEMU exits 0. On FAIL it parks the hart in a wfi loop without
// touching the finisher; the bench harness runs QEMU under
// `timeout`, so a FAIL is observable as the timeout exit status
// (124) in addition to the RESULT: FAIL line.

#include "../uart.h"

// mip bit 3: machine software interrupt pending (MSIP).
#define MIP_MSIP (1UL << 3)
// mip bit 1: supervisor software interrupt pending (SSIP), the
// control bit: software-writable on this QEMU.
#define MIP_SSIP (1UL << 1)
// mie bit 3: machine software interrupt enable (MSIE).
#define MIE_MSIE (1UL << 3)
// mstatus bit 3: machine interrupt enable (MIE).
#define MSTATUS_MIE (1UL << 3)

// Boot baselines measured on QEMU 8.2.2 virt (scratch probe,
// then asserted here).
#define EXPECT_MIP_BOOT     0x80UL         // MTIP pends at boot
#define EXPECT_MIE_BOOT     0x0UL
#define EXPECT_MSTATUS_BOOT 0xa00000000UL  // UXL/SXL = 64-bit
#define EXPECT_MIP_SSIP_SET 0x82UL         // MTIP still set, SSIP set

// Bounded spin window: enough iterations that a delivered
// interrupt could not be missed, still short enough to finish in
// well under the harness timeout.
#define SPIN_ITERS 1000000UL

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final RESULT line from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

volatile unsigned long mmsw_trap_count;

extern void mmsw_trap_entry(void);

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

static unsigned long read_mip(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mip" : "=r"(v));
    return v;
}

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

// Set/clear mip bit 1 (SSIP, the control) with the CSR immediate
// forms: the 5-bit immediate 2 encodes exactly bit 1.
static void set_ssip(void) {
    __asm__ volatile("csrsi mip, 2");
}

static void clear_ssip(void) {
    __asm__ volatile("csrci mip, 2");
}

// Attempt to set/clear mip bit 3 (MSIP) with the CSR immediate
// forms: the 5-bit immediate 8 encodes exactly bit 3.
static void set_msip(void) {
    __asm__ volatile("csrsi mip, 8");
}

static void clear_msip(void) {
    __asm__ volatile("csrci mip, 8");
}

static unsigned long fnv1a_64(unsigned long h, unsigned long v) {
    h ^= v;
    h *= 0x100000001b3UL;
    return h;
}

int main(void) {
    unsigned long mip_boot, mie_boot, mstatus_boot;
    unsigned long mip_ssip_set, mip_ssip_clear;
    unsigned long mip_set, mip_hold, mip_clear;
    unsigned long mie_final, mstatus_final;
    unsigned long csum;
    volatile unsigned long i;

    uart_init();
    uart_puts("mip-msip-write: mip.MSIP software-write probe\n");

    // 1. Boot baselines: MTIP pends at boot, both enables clear.
    mip_boot = read_mip();
    mie_boot = read_mie();
    mstatus_boot = read_mstatus();
    uart_puts("boot: mip=");
    uart_put_hex(mip_boot);
    uart_puts(" mie=");
    uart_put_hex(mie_boot);
    uart_puts(" mstatus=");
    uart_put_hex(mstatus_boot);
    uart_puts("\n");
    check(mip_boot == EXPECT_MIP_BOOT, "mip != 0x80 at boot");
    check(mie_boot == EXPECT_MIE_BOOT, "mie != 0x0 at boot");
    check(mstatus_boot == EXPECT_MSTATUS_BOOT,
          "mstatus != 0xa00000000 at boot");
    check((mstatus_boot & MSTATUS_MIE) == 0, "mstatus.MIE set at boot");
    check((mie_boot & MIE_MSIE) == 0, "mie.MSIE set at boot");

    // Defensive vector only: the enable gates stay clear for the
    // whole run, so no trap should ever fire. The entry counts the
    // trap and parks; reaching the verdict line is the no-trap
    // proof and the printed count is the entry's own counter.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)mmsw_trap_entry));
    {
        unsigned long tv;
        __asm__ volatile("csrr %0, mtvec" : "=r"(tv));
        check((tv & ~3UL) == (unsigned long)mmsw_trap_entry,
              "mtvec did not take the handler address");
        check((tv & 3UL) == 0, "mtvec not in direct mode");
    }

    // 2. Control: the same immediate-form CSR write sets mip.SSIP
    // (bit 1) and reads back, then clears. The write path works;
    // whatever happens to MSIP next is specific to that bit.
    set_ssip();
    mip_ssip_set = read_mip();
    uart_puts("mip: after csrsi set bit 1 readback=");
    uart_put_hex(mip_ssip_set);
    uart_puts(" (expect 0x82)\n");
    check(mip_ssip_set == EXPECT_MIP_SSIP_SET,
          "control: mip readback != 0x82 after SSIP set");
    clear_ssip();
    mip_ssip_clear = read_mip();
    uart_puts("mip: after csrci clear bit 1 readback=");
    uart_put_hex(mip_ssip_clear);
    uart_puts(" (expect 0x80)\n");
    check(mip_ssip_clear == EXPECT_MIP_BOOT,
          "control: mip readback != 0x80 after SSIP clear");

    // 3. Attempt the software write of mip.MSIP (bit 3). On this
    // QEMU the write is ignored: the bit does not set and the
    // readback is unchanged.
    set_msip();
    mip_set = read_mip();
    uart_puts("mip: after csrsi set bit 3 readback=");
    uart_put_hex(mip_set);
    uart_puts(" (expect 0x80, write ignored)\n");
    check((mip_set & MIP_MSIP) == 0, "mip.MSIP set by csrsi");
    check(mip_set == EXPECT_MIP_BOOT,
          "mip changed by ignored MSIP write");

    // 4. Bounded spin window with the interrupt enables still
    // clear: the pending MTIP stays asserted and undelivered, the
    // trap count stays 0.
    for (i = 0; i < SPIN_ITERS; i++)
        __asm__ volatile("" ::: "memory");
    mip_hold = read_mip();
    uart_puts("mip: after spin window readback=");
    uart_put_hex(mip_hold);
    uart_puts(" traps=");
    uart_put_dec(mmsw_trap_count);
    uart_puts(" (expect 0x80 / 0)\n");
    check(mip_hold == EXPECT_MIP_BOOT, "mip changed during spin");
    check(mmsw_trap_count == 0, "trap fired with enables clear");

    // 5. Attempt the software clear of mip.MSIP (bit 3). The write
    // is likewise ignored: the readback is unchanged.
    clear_msip();
    mip_clear = read_mip();
    uart_puts("mip: after csrci clear bit 3 readback=");
    uart_put_hex(mip_clear);
    uart_puts(" (expect 0x80, write ignored)\n");
    check(mip_clear == EXPECT_MIP_BOOT,
          "mip changed by ignored MSIP clear");

    // 6. Final state: enables still clear, no trap ever fired.
    mie_final = read_mie();
    mstatus_final = read_mstatus();
    check((mie_final & MIE_MSIE) == 0, "mie.MSIE set at end");
    check((mstatus_final & MSTATUS_MIE) == 0, "mstatus.MIE set at end");
    check(mmsw_trap_count == 0, "trap fired during the run");

    // Checksum over the deterministic measured values only: every
    // input is a register readback or the trap counter, so it is
    // byte-identical across runs.
    csum = 0xcbf29ce484222325UL;
    csum = fnv1a_64(csum, mip_boot);
    csum = fnv1a_64(csum, mie_boot);
    csum = fnv1a_64(csum, mstatus_boot);
    csum = fnv1a_64(csum, mip_ssip_set);
    csum = fnv1a_64(csum, mip_ssip_clear);
    csum = fnv1a_64(csum, mip_set);
    csum = fnv1a_64(csum, mip_hold);
    csum = fnv1a_64(csum, mip_clear);
    csum = fnv1a_64(csum, mie_final);
    csum = fnv1a_64(csum, mstatus_final);
    csum = fnv1a_64(csum, mmsw_trap_count);

    // Verdict section: deterministic measured values only.
    uart_puts("VERDICT mip_boot=");
    uart_put_hex(mip_boot);
    uart_puts(" mie_boot=");
    uart_put_hex(mie_boot);
    uart_puts(" mip_ssip_set=");
    uart_put_hex(mip_ssip_set);
    uart_puts(" mip_ssip_clear=");
    uart_put_hex(mip_ssip_clear);
    uart_puts(" mip_set=");
    uart_put_hex(mip_set);
    uart_puts(" mip_hold=");
    uart_put_hex(mip_hold);
    uart_puts(" mip_clear=");
    uart_put_hex(mip_clear);
    uart_puts(" mie_final=");
    uart_put_hex(mie_final);
    uart_puts(" traps=");
    uart_put_dec(mmsw_trap_count);
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
