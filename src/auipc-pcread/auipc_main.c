// auipc_main.c: firsthand check that auipc is the window onto the PC.
//
// RISC-V names 32 integer registers, x0-x31. The program counter is not
// one of them: no instruction takes it as a source operand. These three
// checks show that `auipc rd, 0` is the one instruction that puts the
// PC's own value into a register, and that the auipc/ld pair built on
// it reaches data at a link-time offset without knowing the load address.
#include "uart.h"

static const unsigned long marker __attribute__((used)) = 0x123456789abcdef0UL;

int main(void) {
    uart_init();

    // Check 1: auipc rd, 0 returns the address of the auipc itself.
    unsigned long pc0;
    __asm__ volatile("auipc %0, 0" : "=r" (pc0));
    uart_puts("auipc rd,0      -> ");
    uart_put_hex(pc0);
    uart_puts("  (expect the auipc's own address from objdump)\n");

    // Check 2: auipc rd, imm returns PC + (imm << 12).
    unsigned long pc1;
    __asm__ volatile("auipc %0, 0x12345" : "=r" (pc1));
    uart_puts("auipc rd,0x12345 -> ");
    uart_put_hex(pc1);
    uart_puts("  (expect pc + 0x12345000)\n");

    // Check 3 (the applied trick): the auipc/ld pair reaches `marker`
    // through a link-time offset from the live PC. The code never names
    // its own load address.
    unsigned long val;
    __asm__ volatile(
        "1: auipc t0, %%pcrel_hi(marker)\n\t"
        "ld %0, %%pcrel_lo(1b)(t0)\n\t"
        : "=r" (val) :: "t0", "memory");
    uart_puts("pc-relative ld  -> ");
    uart_put_hex(val);
    uart_puts("  (expect 0x123456789abcdef0)\n");

    uart_puts("done\n");
    for (;;) { __asm__ volatile("wfi"); }
    return 0;
}
