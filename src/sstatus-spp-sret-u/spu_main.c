// spu_main.c: sret with sstatus.SPP=0 drops to U-mode
// (backlog item: riscv sstatus-spp-sret-u).
//
// Exactly one mechanism is under test: sstatus.SPP selects the
// privilege mode after sret, so sret with SPP=0 must drop to U-mode.
// The program:
//
//   1. Boots in M-mode (src/boot.S), installs a direct-mode mtvec
//      whose handler records mcause/mepc/mtval/mstatus and jumps to
//      a C continuation (the script ends at the trap; no mret).
//   2. Writes medeleg=0 and reads it back: the illegal-instruction
//      trap below must stay in M-mode, not be delegated.
//   3. Installs a direct-mode stvec whose handler counts S-mode
//      traps and parks: the count must stay 0.
//   4. Opens the whole address space to U-mode with one PMP NAPOT
//      entry, R/W/X (without it, U-mode default-deny raises an
//      instruction access fault on the first fetch).
//   5. Clears sstatus.SPP, reads it back (must be 0), points sepc at
//      the U-mode landing pad, and srets. Never returns to M-mode.
//   6. The landing pad (spu_trap.S) records the address of its
//      privileged read (csrr sstatus) and executes it. U-mode may
//      not read sstatus, so the hart traps to M-mode with
//      mcause=2; the handler records the trap state and jumps to
//      the M-mode continuation.
//   7. The continuation prints the recorded values and a checksum
//      over them, runs 10 checks, and on PASS writes the virt
//      test-device finisher word 0x5555 at 0x100000, which shuts
//      the machine down and QEMU exits 0. On FAIL it prints the
//      failures and parks the hart in a wfi loop; the bench
//      harness runs QEMU under `timeout`, so a FAIL is observable
//      as the timeout exit status (124) in addition to the
//      RESULT: FAIL line.
//
// The proof that the hart was in U-mode is threefold: mcause=2
// (illegal instruction on the privileged CSR read), mepc exactly at
// the read site, and the trapped mstatus.MPP field reading 0
// (U-mode). Had the sret not dropped privilege, the csrr would have
// succeeded in M-mode and no trap would exist at all: the landing
// pad would fall through to its park loop and the run would time
// out instead of printing RESULT.

#include "../uart.h"

static volatile unsigned long m_regs[8];       // M-mode trap scratch, mscratch points here
static volatile unsigned long s_trap_words[2]; // S-mode trap scratch, sscratch points here

// Written by u_landing (spu_trap.S) with the address of the
// privileged read, before the trap fires.
volatile unsigned long expected_site;

static volatile unsigned long g_medeleg;  // medeleg readback, carried to the continuation
static volatile unsigned long g_spp;      // sstatus.SPP readback, carried to the continuation

extern void m_trap_entry(void);
extern void s_trap_entry(void);
extern void u_landing(void);

// M-mode continuation, entered once via the trap handler. Never
// called from C and never returns.
void m_after_trap(void);

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

// FNV-1a 64-bit over the measured values, printed so the proof log
// carries a checksum of the recorded data.
static unsigned long checksum_measured(void) {
    static const int mslots[] = {0, 2, 3, 4, 5};
    unsigned long h = 1469598103934665603UL;
    unsigned int i, b;
    unsigned long words[8];
    words[0] = g_medeleg;
    words[1] = g_spp;
    words[2] = s_trap_words[0];
    for (i = 0; i < sizeof(mslots) / sizeof(mslots[0]); i++)
        words[3 + i] = m_regs[mslots[i]];
    for (i = 0; i < 8; i++) {
        unsigned long w = words[i];
        for (b = 0; b < 8; b++) {
            h ^= (w >> (b * 8)) & 0xffUL;
            h *= 1099511628211UL;
        }
    }
    return h;
}

void m_after_trap(void) {
    unsigned long count = m_regs[0];
    unsigned long mcause = m_regs[2];
    unsigned long mepc = m_regs[3];
    unsigned long mtval = m_regs[4];
    unsigned long mpp = (m_regs[5] >> 11) & 3UL;

    uart_puts("trap: count=");
    uart_put_dec(count);
    uart_puts(" mcause=");
    uart_put_hex(mcause);
    uart_puts(" mepc=");
    uart_put_hex(mepc);
    uart_puts(" mtval=");
    uart_put_hex(mtval);
    uart_puts(" mpp=");
    uart_put_dec(mpp);
    uart_puts("\n");
    uart_puts("site: expected=");
    uart_put_hex(expected_site);
    uart_puts("\n");
    uart_puts("cksum=");
    uart_put_hex(checksum_measured());
    uart_puts("\n");
    uart_puts("s-traps=");
    uart_put_dec(s_trap_words[0]);
    uart_puts("\n");

    check(count == 1, "M-mode trap count is not 1");
    check(mcause == 0x2, "mcause is not 2 (illegal instruction)");
    // mepc must be the privileged read itself: this is the sret-taken
    // check, the trap fired at the U-mode landing pad's csrr.
    check(mepc == expected_site, "mepc is not at the privileged-read site");
    // The trap hardware records the pre-trap privilege in
    // mstatus.MPP: 0 means the hart was in U-mode when it trapped.
    check(mpp == 0, "trapped mstatus.MPP is not 0 (U-mode)");
    // QEMU 8.2.2 writes the faulting instruction word into mtval on
    // an illegal-instruction trap (measured 0x100022f3, the encoding
    // of the csrr sstatus below). Check mtval against the actual
    // trapped word rather than a guessed constant.
    check(mtval == *(volatile unsigned int *)mepc,
          "mtval does not match the word at mepc");
    check(s_trap_words[0] == 0, "a trap reached S-mode");

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
    unsigned long md, sv, ss;

    uart_init();
    uart_puts("sstatus-spp-sret-u: sret with SPP=0 drops to U-mode\n");

    // M-mode trap vector: the one expected trap lands here.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // No delegation: the illegal-instruction trap must be handled in
    // M-mode. Write 0, read back, require 0 (in particular bit 2,
    // illegal instruction, must not be delegated).
    __asm__ volatile("csrw medeleg, %0" :: "r"(0UL));
    __asm__ volatile("csrr %0, medeleg" : "=r"(md));
    g_medeleg = md;
    check(md == 0, "medeleg did not read back 0");
    uart_puts("deleg: medeleg=");
    uart_put_hex(md);
    uart_puts("\n");

    // S-mode trap vector and scratch: installed as a control, the
    // handler counts traps and parks. The count must stay 0.
    __asm__ volatile("csrw stvec, %0" :: "r"((unsigned long)s_trap_entry));
    __asm__ volatile("csrw sscratch, %0" :: "r"((unsigned long)s_trap_words));
    __asm__ volatile("csrr %0, stvec" : "=r"(sv));
    check((sv & ~3UL) == (unsigned long)s_trap_entry,
          "stvec did not take the handler address");
    check((sv & 3UL) == 0, "stvec not in direct mode");

    // PMP: with no PMP entry programmed, lower privilege modes
    // default-deny every address. Open the whole address space to
    // U-mode with one NAPOT entry, R/W/X, before the drop. Without
    // this, the first U-mode instruction fetch raises an
    // instruction access fault instead of reaching the read.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // sstatus.SPP = 0: the sret below must drop to U-mode. Read back
    // to confirm the field is really 0 before the sret.
    __asm__ volatile("csrc sstatus, %0" :: "r"(1UL << 8));
    __asm__ volatile("csrr %0, sstatus" : "=r"(ss));
    g_spp = (ss >> 8) & 1UL;
    check(g_spp == 0, "sstatus.SPP did not read back 0");
    uart_puts("spp: readback=");
    uart_put_dec(g_spp);
    uart_puts("\n");

    __asm__ volatile("csrw sepc, %0\n"
                     "sret" :: "r"((unsigned long)u_landing) : "memory");
    __builtin_unreachable();
}
