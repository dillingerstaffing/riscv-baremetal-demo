// stu_main.c: sret executed in U-mode raises illegal-instruction
// (backlog item: riscv sret-to-umode-fault).
//
// Exactly one mechanism is under test: sret is a privileged
// instruction, executable only in S-mode or M-mode, so executing it
// in U-mode must trap to M-mode with mcause=2 (illegal
// instruction), mepc at the sret, and mstatus.MPP=0 (the trap
// hardware records the pre-trap privilege). The program:
//
//   1. Boots in M-mode (src/boot.S), installs a direct-mode mtvec
//      whose handler records mcause/mepc/mtval/mstatus on every
//      M-mode trap. Two traps are scripted:
//        a. the phase-1 S-mode ecall (mcause 0x9): recorded, then
//           redirected to phase2_drop in M-mode via mret;
//        b. anything else (the phase-2 U-mode sret trap): recorded,
//           then the handler jumps to the C continuation; the
//           script ends at the trap, no mret.
//   2. Writes medeleg=0 and mie=0 and reads both back: every trap
//      must stay in M-mode, and no interrupt may fire. Reads back
//      mstatus.MIE clear as well.
//   3. Installs a direct-mode stvec whose handler counts S-mode
//      traps and parks: the count must stay 0.
//   4. Opens the whole address space to S/U-mode with one PMP NAPOT
//      entry, R/W/X (without it, lower-privilege fetches fault).
//   5. Phase 1, the control: sret from M-mode with SPP=1 and sepc
//      at s_landing (sret is legal in M-mode). The S-mode pad
//      flags arrival, then executes sret in S-mode with SPP=1,
//      which must return to S-mode with no trap, then issues the
//      one scripted ecall back to M-mode.
//   6. Phase 2: the ecall path mrets to phase2_drop, which srets
//      from M-mode with SPP=0, dropping to U-mode. The U-mode
//      landing pad records the address of its sret and executes
//      it. U-mode may not execute sret, so the hart traps to
//      M-mode with mcause=2; the handler records the trap state
//      and jumps to the M-mode continuation.
//   7. The continuation prints the recorded values and a checksum
//      over them, runs the checks, and on PASS writes the virt
//      test-device finisher word 0x5555 at 0x100000, which shuts
//      the machine down and QEMU exits 0. On FAIL it prints the
//      failures and parks the hart in a wfi loop; the bench
//      harness runs QEMU under `timeout`, so a FAIL is observable
//      as the timeout exit status (124) in addition to the
//      RESULT: FAIL line.
//
// The proof that the sret trapped from U-mode is fourfold:
// mcause=2, mepc exactly at the sret site recorded by the pad,
// the trapped mstatus.MPP field reading 0 (U-mode), and mtval
// holding the faulting instruction word. Had the hart not been in
// U-mode, or had sret been legal there, the pad would fall through
// to its park loop and the run would time out instead of printing
// RESULT.

#include "../uart.h"

static volatile unsigned long m_regs[8];       // M-mode trap scratch, mscratch points here
static volatile unsigned long s_trap_words[2]; // S-mode trap scratch, sscratch points here

// Written by the landing pads (stu_trap.S) before the traps fire.
volatile unsigned long expected_site;      // address of the U-mode sret
volatile unsigned long control_sret_site;  // address of the S-mode sret (control)
volatile unsigned long control_ok;         // S-mode pad reached
volatile unsigned long control_after;      // S-mode sret returned without trapping
volatile unsigned long ecall_site;         // address of the scripted S-mode ecall
volatile unsigned long ecall_cause;        // recorded by the handler on the ecall path
volatile unsigned long ecall_epc;          // recorded by the handler on the ecall path
volatile unsigned int g_expect_ecall;      // 1 while the phase-1 ecall is outstanding

static volatile unsigned long g_medeleg;  // medeleg readback, carried to the continuation
static volatile unsigned long g_mie;      // mie readback, carried to the continuation
static volatile unsigned long g_mstatus;  // mstatus readback after MIE clear

extern void m_trap_entry(void);
extern void s_trap_entry(void);
extern void s_landing(void);

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

// FNV-1a 64-bit over the measured values, printed so the proof log
// carries a checksum of the recorded data. Only verdict-relevant
// values feed it; raw addresses enter only as equality bits, so the
// checksum is identical across runs.
static unsigned long checksum_measured(void) {
    unsigned long h = 1469598103934665603UL;
    unsigned int i, b;
    unsigned long words[12];
    words[0] = g_medeleg;
    words[1] = g_mie;
    words[2] = (g_mstatus >> 3) & 1UL;      // MIE bit after clear
    words[3] = control_ok;
    words[4] = control_after;
    words[5] = ecall_cause;
    words[6] = (ecall_epc == ecall_site) ? 1UL : 0UL;
    words[7] = m_regs[0];                   // M-mode trap count
    words[8] = m_regs[2];                   // mcause
    words[9] = (m_regs[3] == expected_site) ? 1UL : 0UL;
    words[10] = (m_regs[5] >> 11) & 3UL;     // trapped MPP
    words[11] = s_trap_words[0];            // S-mode trap count
    for (i = 0; i < 12; i++) {
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
    unsigned long word_at_mepc = *(volatile unsigned int *)mepc;

    uart_puts("phase1: control_ok=");
    uart_put_dec(control_ok);
    uart_puts(" control_after=");
    uart_put_dec(control_after);
    uart_puts(" ecall_cause=");
    uart_put_hex(ecall_cause);
    uart_puts(" ecall_epc=");
    uart_put_hex(ecall_epc);
    uart_puts(" ecall_site=");
    uart_put_hex(ecall_site);
    uart_puts("\n");
    uart_puts("phase2: count=");
    uart_put_dec(count);
    uart_puts(" mcause=");
    uart_put_hex(mcause);
    uart_puts(" mepc=");
    uart_put_hex(mepc);
    uart_puts(" mtval=");
    uart_put_hex(mtval);
    uart_puts(" mpp=");
    uart_put_dec(mpp);
    uart_puts(" expected=");
    uart_put_hex(expected_site);
    uart_puts("\n");
    uart_puts("phase2: s_traps=");
    uart_put_dec(s_trap_words[0]);
    uart_puts(" cksum=");
    uart_put_hex(checksum_measured());
    uart_puts("\n");

    // Control phase: the S-mode sret must have executed and
    // returned to S-mode without trapping.
    check(control_ok == 1, "control: S-mode landing pad never ran");
    check(control_after == 1,
          "control: S-mode sret did not return (it trapped)");
    check(control_sret_site != 0,
          "control: S-mode sret site was not recorded");
    check(ecall_cause == 0x9UL,
          "control: phase-1 return was not an S-mode ecall");
    check(ecall_epc == ecall_site,
          "control: ecall mepc is not at the recorded ecall site");
    // Core claim: the U-mode sret trapped to M-mode as illegal.
    check(count == 2, "M-mode trap count is not 2");
    check(mcause == 0x2, "mcause is not 2 (illegal instruction)");
    check(mepc == expected_site,
          "mepc is not at the U-mode sret site");
    check(mpp == 0, "trapped mstatus.MPP is not 0 (U-mode)");
    // QEMU 8.2.2 writes the faulting instruction word into mtval
    // on an illegal-instruction trap. Check mtval against the
    // actual trapped word rather than a guessed constant.
    check(mtval == word_at_mepc,
          "mtval does not match the word at mepc");
    check(s_trap_words[0] == 0, "a trap reached S-mode");
    // Setup invariants.
    check(g_medeleg == 0, "medeleg did not read back 0");
    check(g_mie == 0, "mie did not read back 0");
    check(((g_mstatus >> 3) & 1UL) == 0, "mstatus.MIE was not clear");

    uart_puts("Checks: ");
    uart_put_dec(checks);
    uart_puts("\nMismatches: ");
    uart_put_dec(fails);
    uart_puts("\nChecksum: ");
    uart_put_hex(checksum_measured());
    uart_puts("\nEnvironment: QEMU 8.2.2\n");
    if (fails == 0)
        uart_puts("Verdict: PASS\n");
    else {
        uart_puts("Verdict: FAIL (");
        uart_put_dec((unsigned long)fails);
        uart_puts(" checks failed)\n");
    }
    uart_drain();

    if (fails == 0) {
        *VIRT_TEST_FINISHER = FINISHER_PASS;
    }
    for (;;)
        __asm__ volatile("wfi");
}

int main(void) {
    unsigned long md, sv, mi, ms;

    uart_init();
    uart_puts("sret-to-umode-fault: sret in U-mode traps illegal-instruction\n");

    // M-mode trap vector: the two scripted traps land here.
    __asm__ volatile("csrw mtvec, %0" :: "r"((unsigned long)m_trap_entry));
    __asm__ volatile("csrw mscratch, %0" :: "r"((unsigned long)m_regs));

    // No delegation: every trap must be handled in M-mode. Write 0,
    // read back, require 0 (in particular bit 2, illegal
    // instruction, must not be delegated).
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

    // Disarm the M-mode interrupt path: mie clear, mstatus.MIE
    // clear, so the scripted synchronous traps are the only traps
    // that can fire.
    __asm__ volatile("csrw mie, %0" :: "r"(0UL));
    __asm__ volatile("csrr %0, mie" : "=r"(mi));
    g_mie = mi;
    check(mi == 0, "mie did not read back 0");
    __asm__ volatile("csrci mstatus, 8");  // MIE off
    __asm__ volatile("csrr %0, mstatus" : "=r"(ms));
    g_mstatus = ms;

    // PMP: with no PMP entry programmed, lower privilege modes
    // default-deny every address. Open the whole address space to
    // S/U-mode with one NAPOT entry, R/W/X, before the drops.
    // Without this, the first S-mode instruction fetch raises an
    // instruction access fault instead of reaching the pads.
    __asm__ volatile("li t0, -1\n"
                     "csrw pmpaddr0, t0\n"
                     "li t0, 0x1f\n"      // A=NAPOT, R/W/X
                     "csrw pmpcfg0, t0" ::: "t0");

    // Phase 1, the control: sret from M-mode with SPP=1 and sepc at
    // the S-mode landing pad. sret is legal in M-mode, so this
    // drops to S-mode without trapping. Never returns to main.
    g_expect_ecall = 1;
    __asm__ volatile("csrw sepc, %0\n"
                     "csrs sstatus, %1\n"
                     "sret"
                     :: "r"((unsigned long)s_landing), "r"(1UL << 8)
                     : "memory");
    __builtin_unreachable();
}
