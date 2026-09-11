// ssp_main.c: sstatus.SPP record on delegated S-mode ecall traps
// (backlog item 174).
//
// Exactly one mechanism is under test: sstatus.SPP records the
// privilege mode the hart was in before a trap. The program:
//
//   1. Boots in M-mode (src/boot.S), installs a direct-mode mtvec
//      whose handler records mcause and parks the hart: no trap may
//      reach M-mode during the run.
//   2. Sets medeleg bit 9 (supervisor environment call) so S-mode
//      ecalls trap to S-mode, and reads medeleg back to confirm the
//      bit stuck.
//   3. Installs a direct-mode stvec (S-mode handler in ssp_trap.S),
//      points sscratch at the trap scratch array, opens the whole
//      address space to S-mode with one PMP NAPOT entry, sets
//      sstatus.SPP=1 and mstatus.MPP=1 (S-mode), points sepc at the
//      S-mode payload, and srets. Never returns to M-mode.
//   4. The S-mode payload issues two ecalls. Each trap must enter the
//      S-mode handler with sstatus.SPP=1 (the pre-trap mode) and
//      scause=9; the handler records both, advances sepc by 4, and
//      srets back to S-mode.
//   5. The payload prints the recorded SPP/scause/sepc values for
//      both traps, a checksum over them, runs 10 checks, and on PASS
//      writes the virt test-device finisher word 0x5555 at 0x100000,
//      which shuts the machine down and QEMU exits 0. On FAIL it
//      prints the failures and parks the hart in a wfi loop; the
//      bench harness runs QEMU under `timeout`, so a FAIL is
//      observable as the timeout exit status (124) in addition to
//      the RESULT: FAIL line.
//
// Reaching the S-mode handler at all proves the sret dropped to
// S-mode: an M-mode ecall would report mcause 11 and trap to the
// M-mode handler (medeleg only delegates cause 9), which parks.

#include "../uart.h"

static volatile unsigned long ssp_regs[16];  // S-mode trap scratch, sscratch points here
static volatile unsigned long m_regs[8];     // M-mode trap scratch, mscratch points here

extern void s_trap_entry(void);
extern void m_trap_entry(void);

// S-mode payload, entered once via sret from main. Never called from C.
void s_payload(void);

#define VIRT_TEST_FINISHER ((volatile unsigned int *)0x100000UL)
#define FINISHER_PASS 0x5555u

// ns16550a line status: TEMT (bit 6) reads 1 when both the holding
// register and the shift register are empty, i.e. the last byte is
// fully out on the wire. Polling it before the finisher write keeps
// the final lines from being cut off by the shutdown.
#define UART0_LSR ((volatile unsigned char *)0x10000005UL)
#define LSR_TEMT (1 << 6)

static void uart_drain(void) {
    while ((*UART0_LSR & LSR_TEMT) == 0)
        ;
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

// FNV-1a 64-bit over the recorded per-trap values, printed so the
// proof log carries a checksum of the measured data.
static unsigned long checksum_traps(void) {
    static const int slots[] = {6, 7, 8, 9, 10, 11};
    unsigned long h = 1469598103934665603UL;
    unsigned int i, b;
    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        unsigned long w = ssp_regs[slots[i]];
        for (b = 0; b < 8; b++) {
            h ^= (w >> (b * 8)) & 0xffUL;
            h *= 1099511628211UL;
        }
    }
    return h;
}

// One synchronous trap from S-mode. The handler in ssp_trap.S records
// scause/SPP/sepc, skips the ecall (sepc += 4), and returns; this
// function returns only after the trap resolved.
__attribute__((noinline)) static void trap_once(void) {
    __asm__ volatile("ecall" ::: "memory");
}

void s_payload(void) {
    unsigned long spp1, scause1, sepc1, spp2, scause2, sepc2;

    trap_once();  // trap 1
    trap_once();  // trap 2

    spp1 = ssp_regs[6];
    scause1 = ssp_regs[7];
    sepc1 = ssp_regs[8];
    spp2 = ssp_regs[9];
    scause2 = ssp_regs[10];
    sepc2 = ssp_regs[11];

    uart_puts("trap1: spp=");
    uart_put_dec(spp1);
    uart_puts(" scause=");
    uart_put_hex(scause1);
    uart_puts(" sepc=");
    uart_put_hex(sepc1);
    uart_puts("\n");
    uart_puts("trap2: spp=");
    uart_put_dec(spp2);
    uart_puts(" scause=");
    uart_put_hex(scause2);
    uart_puts(" sepc=");
    uart_put_hex(sepc2);
    uart_puts("\n");
    uart_puts("cksum=");
    uart_put_hex(checksum_traps());
    uart_puts("\n");
    uart_puts("m-traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");

    check(ssp_regs[0] == 2, "S-mode trap count is not 2");
    check(spp1 == 1, "trap1 SPP is not 1");
    check(scause1 == 9, "trap1 scause is not 9");
    check(spp2 == 1, "trap2 SPP is not 1");
    check(scause2 == 9, "trap2 scause is not 9");
    // Both traps come from the one ecall inside trap_once, executed
    // twice, so the recorded sepc values must be equal; and the word
    // at that address must be the ecall encoding 0x00000073, proving
    // the recorded sepc is the trapping instruction itself.
    check(sepc1 == sepc2, "trap sepc values differ");
    check(*(volatile unsigned int *)sepc1 == 0x73,
          "word at trap sepc is not ecall");
    check(m_regs[0] == 0, "a trap reached M-mode");

    if (fails == 0) {
        uart_puts("RESULT: PASS\n");
    } else {
        uart_puts("RESULT: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_puts("done\n");
    uart_drain();

    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;
    }
    for (;;)
        __asm__ volatile("wfi");
}

int main(void) {
    unsigned long md, sv;

    uart_init();
    uart_puts("sstatus-spp: sstatus.SPP record on delegated S-mode ecall traps\n");

    // M-mode trap vector: any trap reaching M-mode is a hard failure.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // Delegate supervisor environment calls (exception code 9) to
    // S-mode. Read back to confirm the bit stuck.
    __asm__ volatile("csrr %0, medeleg" : "=r"(md));
    md |= (1UL << 9);
    __asm__ volatile("csrw medeleg, %0" :: "r"(md));
    __asm__ volatile("csrr %0, medeleg" : "=r"(md));
    check((md & (1UL << 9)) != 0, "medeleg bit 9 did not stick");
    uart_puts("deleg: medeleg=");
    uart_put_hex(md);
    uart_puts("\n");

    // S-mode trap vector and scratch.
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)ssp_regs));
    __asm__ volatile("csrr %0, stvec" : "=r"(sv));
    check((sv & ~3UL) == (unsigned long)s_trap_entry,
          "stvec did not take the handler address");
    check((sv & 3UL) == 0, "stvec not in direct mode");

    // PMP: with no PMP entry programmed, S-mode has no access to any
    // address (lower modes default-deny). Open the whole address
    // space to S-mode with one NAPOT entry, R/W/X, before the drop.
    // Without this, the first S-mode instruction fetch raises an
    // instruction access fault.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // sstatus.SPP = 1 and mstatus.MPP = 1 (S-mode): the sret below
    // drops to S-mode. The hardware sets sstatus.SPP to the pre-trap
    // mode on the next trap, which is what the payload verifies.
    __asm__ volatile("csrs sstatus, %0" :: "r"(1UL << 8));
    __asm__ volatile("csrc mstatus, %0" :: "r"(3UL << 11));
    __asm__ volatile("csrs mstatus, %0" :: "r"(1UL << 11));

    __asm__ volatile("csrw sepc, %0\n"
                     "sret" :: "r"((unsigned long)s_payload) : "memory");
    __builtin_unreachable();
}
