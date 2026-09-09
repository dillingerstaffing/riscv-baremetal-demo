// plic_main.c: PLIC claim/complete round-trip (backlog item 32).
//
// Drives the platform-level interrupt controller on the QEMU virt board
// directly, through its memory-mapped registers. Context 0 of the PLIC
// is hart 0's M-mode context, and interrupt source 10 is UART0 on the
// virt machine.
//
// Sequence under test:
//   1. Claim with nothing pending: must return 0 (control).
//   2. Program priority[10]=1, enable bit 10 in context 0, threshold 0.
//      Each write is read back and checked.
//   3. Assert the interrupt: put the UART in internal loopback mode,
//      enable its receive-data interrupt, and write one byte to the
//      transmitter holding register. The byte loops back into the
//      receiver, the UART raises its IRQ line, and the PLIC sets the
//      pending bit for source 10. The pending bit is read directly
//      from the PLIC pending register, not assumed.
//   4. Read the claim register (timed with rdcycle). It must return
//      10, and the claim must clear the pending bit (verified by
//      reading the pending register again).
//   5. Read the looped-back byte out of the UART (this drops the
//      UART's own interrupt condition, a level-triggered source would
//      re-pend otherwise), then write 10 to the claim/complete
//      register (timed with rdcycle).
//   6. Claim again: with nothing pending it must return 0, proving
//      the complete step returned the source to the idle state.
//
// No trap handler is installed and mie/mstatus are never touched: the
// PLIC's claim/complete semantics are exercised by polling its
// registers, which keeps the mechanism under test exactly one thing
// (the claim/complete round trip) and nothing else.

#include "../uart.h"

// QEMU virt PLIC, base 0x0c000000. Context 0 is hart 0's M-mode
// context. Register layout follows the PLIC specification.
#define PLIC_BASE     0x0c000000UL
#define PLIC_PRIO(s)  (PLIC_BASE + 4UL * (unsigned long)(s))
#define PLIC_PENDING  (PLIC_BASE + 0x1000UL)    // word 0: sources 0..31
#define PLIC_ENABLE   (PLIC_BASE + 0x2000UL)    // ctx 0, word 0
#define PLIC_THRESH   (PLIC_BASE + 0x200000UL)  // ctx 0 threshold
#define PLIC_CLAIM    (PLIC_BASE + 0x200004UL)  // ctx 0 claim/complete

#define UART_IRQ 10  // UART0 interrupt source on the virt machine

// UART0 (ns16550a) registers used to assert the interrupt.
#define UART0_BASE 0x10000000UL
#define U_THR  0x00  // transmitter holding register (write); RBR (read)
#define U_IER  0x01  // interrupt enable register
#define U_MCR  0x04  // modem control register
#define U_LSR  0x05  // line status register
#define IER_RDI  0x01  // received-data-available interrupt enable
#define MCR_LOOP 0x10  // internal loopback mode
#define LSR_DR   0x01  // receiver data ready
#define LSR_THRE 0x20  // transmitter holding register empty

static volatile unsigned int *plic_reg(unsigned long addr) {
    return (volatile unsigned int *)addr;
}

static volatile unsigned char *uart_reg(unsigned long off) {
    return (volatile unsigned char *)(UART0_BASE + off);
}

static unsigned long rdcycle(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, cycle" : "=r"(v));
    return v;
}

static unsigned long csr_read_mhartid(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return v;
}

#define CLINT_MTIME 0x0200bff8UL  // 64-bit mtime, 10 MHz timebase

static unsigned long clint_get_mtime(void) {
    return *(volatile unsigned long *)CLINT_MTIME;
}

// Calibration: rdcycle on this QEMU follows the host clock, not the
// 10 MHz mtime timebase. Spin for 1M mtime ticks (100 ms of virtual
// time) and report rdcycle units per tick, so the claim/complete
// cycle counts below are interpretable as host time. (Same
// construction as src/wfi-latency/.)
static unsigned long calibration(void) {
    unsigned long t0, t1, c0, c1, ratio;

    t0 = clint_get_mtime();
    c0 = rdcycle();
    while (clint_get_mtime() - t0 < 1000000UL)
        ;
    t1 = clint_get_mtime();
    c1 = rdcycle();
    ratio = (c1 - c0) / (t1 - t0);
    uart_puts("clock: 1000000 mtime ticks -> rdcycle delta ");
    uart_put_dec(c1 - c0);
    uart_puts(" (ratio ");
    uart_put_dec(ratio);
    uart_puts(" rdcycle units per tick)\n");
    return ratio;
}

static int fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_puts("\n");
        fails++;
    }
}

int main(void) {
    unsigned int v;
    unsigned long t0, t1;
    unsigned int claimed, claimed2;
    unsigned long claim_cycles, complete_cycles;
    unsigned int pend_before, pend_after, pend_idle;
    unsigned char rx, mcr_save, ier_save;
    unsigned long spins, ratio;

    uart_init();
    uart_puts("plic-claim: PLIC claim/complete round-trip test\n");
    uart_puts("hart mhartid=");
    uart_put_dec(csr_read_mhartid());
    uart_puts("\n");

    ratio = calibration();
    check(ratio >= 145 && ratio <= 155,
          "rdcycle/mtime ratio outside 145..155");

    // Control 1: claim with nothing pending or enabled must return 0.
    claimed = *plic_reg(PLIC_CLAIM);
    uart_puts("control: claim before programming=");
    uart_put_dec(claimed);
    uart_puts(" (expect 0)\n");
    check(claimed == 0, "claim with nothing pending != 0");

    // Program the PLIC: priority 1 for the UART source, enable it in
    // context 0 (hart 0 M-mode), threshold 0 so any nonzero priority
    // source wins. Each write is read back.
    *plic_reg(PLIC_PRIO(UART_IRQ)) = 1;
    v = *plic_reg(PLIC_ENABLE);
    *plic_reg(PLIC_ENABLE) = v | (1U << UART_IRQ);
    *plic_reg(PLIC_THRESH) = 0;
    uart_puts("config: priority[10]=");
    uart_put_dec(*plic_reg(PLIC_PRIO(UART_IRQ)));
    uart_puts(" enable10=");
    uart_put_dec((*plic_reg(PLIC_ENABLE) >> UART_IRQ) & 1U);
    uart_puts(" thresh=");
    uart_put_dec(*plic_reg(PLIC_THRESH));
    uart_puts("\n");
    check(*plic_reg(PLIC_PRIO(UART_IRQ)) == 1, "priority[10] readback != 1");
    check(((*plic_reg(PLIC_ENABLE) >> UART_IRQ) & 1U) == 1,
          "enable bit 10 readback != 1");
    check(*plic_reg(PLIC_THRESH) == 0, "threshold readback != 0");

    // Assert the interrupt. Save IER/MCR, enable internal loopback and
    // the receive-data interrupt, then write one byte to the
    // transmitter. The byte loops back into the receiver, the UART
    // asserts its IRQ line, and the PLIC sets pending bit 10.
    //
    // Nothing is printed while loopback is enabled: in loopback mode
    // the UART model routes transmitted bytes to its own receiver
    // instead of the console, so any output in that window would be
    // silently swallowed.
    //
    // Two ordering constraints, both verified empirically on this
    // QEMU build:
    // - Loopback is switched back off as soon as the pending bit is
    //   observed, before any printing. The received byte is already
    //   sitting in the receiver, so the UART's IRQ line stays
    //   asserted.
    // - The UART interrupt (IER_RDI) stays enabled, keeping the IRQ
    //   line asserted, until the claim is done. This QEMU's PLIC
    //   model clears a source's pending bit when its input line
    //   falls, so dropping the line before the claim would erase the
    //   very state the claim is meant to observe.
    if ((*uart_reg(U_LSR) & LSR_DR) != 0)
        (void)*uart_reg(U_THR);  // drain any stale receive byte
    mcr_save = *uart_reg(U_MCR);
    ier_save = *uart_reg(U_IER);
    *uart_reg(U_MCR) = (unsigned char)(mcr_save | MCR_LOOP);
    *uart_reg(U_IER) = (unsigned char)(ier_save | IER_RDI);
    while ((*uart_reg(U_LSR) & LSR_THRE) == 0)
        ;  // wait for an empty transmitter
    *uart_reg(U_THR) = 'Q';

    // The loopback path is synchronous in the UART model, but poll
    // with a cycle budget anyway so a silent model never hangs the
    // test; a timeout is a FAIL, not a hang.
    spins = 0;
    while ((((*plic_reg(PLIC_PENDING) >> UART_IRQ) & 1U) == 0) &&
           spins < 1000000UL)
        spins++;
    pend_before = *plic_reg(PLIC_PENDING);

    // Loopback off: every report from here on reaches the console.
    // IER is deliberately left with RDI enabled so the IRQ line
    // stays asserted through the claim below.
    *uart_reg(U_MCR) = mcr_save;

    uart_puts("assert: pending bit 10 after TX byte=");
    uart_put_dec((pend_before >> UART_IRQ) & 1U);
    uart_puts(" (spins=");
    uart_put_dec(spins);
    uart_puts(")\n");
    check(spins < 1000000UL, "UART IRQ never showed pending");
    check(((pend_before >> UART_IRQ) & 1U) == 1,
          "pending bit 10 not set after TX byte");

    // Claim, timed. Must return the UART source number. The pending
    // register is read immediately after, before any further UART
    // activity: this QEMU's PLIC model drives each pending bit from
    // its source's input level (hw/intc/sifive_plic.c,
    // sifive_plic_irq_request sets pending from the input level on
    // every line change), so transmit activity while the UART IRQ
    // line is still asserted would set the bit again. The claim
    // itself clears the pending bit and marks the source claimed
    // (set_claimed), which is what suppresses a second claim.
    t0 = rdcycle();
    claimed = *plic_reg(PLIC_CLAIM);
    t1 = rdcycle();
    claim_cycles = t1 - t0;
    pend_after = *plic_reg(PLIC_PENDING);
    uart_puts("claim: id=");
    uart_put_dec(claimed);
    uart_puts(" cycles=");
    uart_put_dec(claim_cycles);
    uart_puts(" pending-after=");
    uart_put_dec((pend_after >> UART_IRQ) & 1U);
    uart_puts("\n");
    check(claimed == UART_IRQ, "claim did not return the UART IRQ");
    check(((pend_after >> UART_IRQ) & 1U) == 0,
          "pending bit 10 still set immediately after claim");

    // Read the looped-back byte out of the receiver. This drops the
    // UART's IRQ line, and since the PLIC's pending bit follows the
    // input level it must read 0 now. The byte itself is ground truth
    // for the loopback path.
    rx = *uart_reg(U_THR);
    pend_idle = *plic_reg(PLIC_PENDING);
    uart_puts("uart: looped-back byte=");
    uart_put_hex(rx);
    uart_puts(" (expect 0x51) pending-after-deassert=");
    uart_put_dec((pend_idle >> UART_IRQ) & 1U);
    uart_puts("\n");
    check(rx == 'Q', "looped-back byte != 'Q'");
    check(((pend_idle >> UART_IRQ) & 1U) == 0,
          "pending bit 10 still set after source deasserted");

    // Complete, timed: write the claimed ID back.
    t0 = rdcycle();
    *plic_reg(PLIC_CLAIM) = UART_IRQ;
    t1 = rdcycle();
    complete_cycles = t1 - t0;
    uart_puts("complete: cycles=");
    uart_put_dec(complete_cycles);
    uart_puts("\n");

    // With the source completed and idle, a second claim must return 0.
    claimed2 = *plic_reg(PLIC_CLAIM);
    uart_puts("reclaim: id=");
    uart_put_dec(claimed2);
    uart_puts(" (expect 0)\n");
    check(claimed2 == 0, "claim after complete != 0");

    // Full UART restore: IER back to its pre-test value (MCR was
    // already restored right after the assert window).
    *uart_reg(U_IER) = ier_save;

    if (fails == 0) {
        uart_puts("RESULT: PASS (claim=10, claim_cycles=");
        uart_put_dec(claim_cycles);
        uart_puts(", complete_cycles=");
        uart_put_dec(complete_cycles);
        uart_puts(")\n");
    } else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    for (;;)
        __asm__ volatile("wfi");
}
