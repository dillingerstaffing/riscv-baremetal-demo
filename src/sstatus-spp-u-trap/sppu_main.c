// sppu_main.c: sstatus.SPP record on a delegated U-mode ecall trap.
//
// Exactly one mechanism is under test: sstatus.SPP records the
// privilege mode the hart was in before a trap. The program:
//
//   1. Boots in M-mode (src/boot.S), installs a direct-mode mtvec
//      whose handler records mcause and parks the hart: no trap may
//      reach M-mode during the run.
//   2. Records the boot medeleg value, sets medeleg bit 8 (user
//      environment call) so U-mode ecalls trap to S-mode, and reads
//      medeleg back to confirm the bit stuck.
//   3. Installs a direct-mode stvec (S-mode handler in sppu_trap.S),
//      points sscratch at the trap scratch array, opens the whole
//      address space to lower modes with one PMP NAPOT entry,
//      clears sstatus.SPP, sets mstatus.MPP=0 (U-mode), points mepc
//      at the U-mode payload, and mrets. Never returns to M-mode.
//   4. The U-mode payload issues one ecall. The trap must enter the
//      S-mode handler with sstatus.SPP=0 (the pre-trap mode) and
//      scause=8; the handler records both, advances sepc by 4, and
//      srets back to U-mode.
//   5. The payload prints the recorded SPP/scause/sepc values, a
//      checksum over them, runs 8 checks, and on PASS writes the
//      virt test-device finisher word 0x5555 at 0x100000, which shuts
//      the machine down and QEMU exits 0. On FAIL it prints the
//      failures and parks the hart in a wfi loop; the bench harness
//      runs QEMU under `timeout`, so a FAIL is observable as the
//      timeout exit status (124) in addition to the RESULT: FAIL
//      line.
//
// Reaching the S-mode handler at all proves the mret dropped to
// U-mode: an M-mode ecall would report mcause 11 and trap to the
// M-mode handler (medeleg only delegates cause 8), which parks.

#include "../uart.h"

static volatile unsigned long spu_regs[16];  // S-mode trap scratch, sscratch points here
static volatile unsigned long m_regs[8];     // M-mode trap scratch, mscratch points here

extern void s_trap_entry(void);
extern void m_trap_entry(void);

// U-mode payload, entered once via mret from main. Never called from C.
void u_payload(void);

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

// FNV-1a 64-bit over the recorded trap values, printed so the proof
// log carries a checksum of the measured data.
static unsigned long checksum_traps(void) {
    static const int slots[] = {6, 7, 8};
    unsigned long h = 1469598103934665603UL;
    unsigned int i, b;
    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        unsigned long w = spu_regs[slots[i]];
        for (b = 0; b < 8; b++) {
            h ^= (w >> (b * 8)) & 0xffUL;
            h *= 1099511628211UL;
        }
    }
    return h;
}

// One synchronous trap from U-mode. The handler in sppu_trap.S
// records scause/SPP/sepc, skips the ecall (sepc += 4), and returns;
// this function returns only after the trap resolved.
__attribute__((noinline)) static void trap_once(void) {
    __asm__ volatile("ecall" ::: "memory");
}

void u_payload(void) {
    unsigned long spp, scause, sepc;

    trap_once();  // the one scripted trap

    spp = spu_regs[6];
    scause = spu_regs[7];
    sepc = spu_regs[8];

    uart_puts("trap: spp=");
    uart_put_dec(spp);
    uart_puts(" scause=");
    uart_put_hex(scause);
    uart_puts(" sepc=");
    uart_put_hex(sepc);
    uart_puts("\n");
    uart_puts("cksum=");
    uart_put_hex(checksum_traps());
    uart_puts("\n");
    uart_puts("m-traps=");
    uart_put_dec(m_regs[0]);
    uart_puts("\n");

    check(spu_regs[0] == 1, "S-mode trap count is not 1");
    check(spp == 0, "trap SPP is not 0");
    check(scause == 8, "trap scause is not 8");
    // The recorded sepc must be the trapping instruction itself:
    // the word there is the ecall encoding 0x00000073.
    check(*(volatile unsigned int *)sepc == 0x73,
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
    unsigned long md_boot, md, sv;

    uart_init();
    uart_puts("sstatus-spp-u-trap: sstatus.SPP record on delegated U-mode ecall trap\n");

    // M-mode trap vector: any trap reaching M-mode is a hard failure.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // Record the boot medeleg, then delegate user environment calls
    // (exception code 8) to S-mode. Read back to confirm the bit
    // stuck.
    __asm__ volatile("csrr %0, medeleg" : "=r"(md_boot));
    md = md_boot | (1UL << 8);
    __asm__ volatile("csrw medeleg, %0" :: "r"(md));
    __asm__ volatile("csrr %0, medeleg" : "=r"(md));
    check((md & (1UL << 8)) != 0, "medeleg bit 8 did not stick");
    uart_puts("deleg: boot_medeleg=");
    uart_put_hex(md_boot);
    uart_puts(" medeleg=");
    uart_put_hex(md);
    uart_puts("\n");

    // S-mode trap vector and scratch.
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)spu_regs));
    __asm__ volatile("csrr %0, stvec" : "=r"(sv));
    check((sv & ~3UL) == (unsigned long)s_trap_entry,
          "stvec did not take the handler address");
    check((sv & 3UL) == 0, "stvec not in direct mode");

    // PMP: with no PMP entry programmed, lower modes default-deny on
    // every address. Open the whole address space to S-mode and
    // U-mode with one NAPOT entry, R/W/X, before the drop. Without
    // this, the first U-mode instruction fetch raises an
    // instruction access fault.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // Clear sstatus.SPP and set mstatus.MPP=0 (U-mode): the mret
    // below drops to U-mode. Taking the address of the C function
    // u_payload is exact; the labels-as-values hazard the project
    // notes warn about applies only to local labels inside a
    // function body at -O2, not to function addresses.
    __asm__ volatile("csrc sstatus, %0" :: "r"(1UL << 8));
    __asm__ volatile("csrc mstatus, %0" :: "r"(3UL << 11));  // MPP = 0

    __asm__ volatile("csrw mepc, %0\n"
                     "mret" :: "r"((unsigned long)u_payload) : "memory");
    __builtin_unreachable();
}
