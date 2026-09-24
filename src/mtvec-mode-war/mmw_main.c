// mmw_main.c: mtvec MODE WARL write/readback probe.
//
// Mechanism under test: the legalization behavior of the mtvec CSR
// on writes to its MODE field (bits 1:0) and on an all-ones write.
// mtvec is a WARL register: a written value must read back
// legalized, with MODE restricted to the modes the implementation
// provides and BASE kept aligned. The run measures exactly what
// this implementation does with each write:
//
//   Phase 0: record the boot mtvec readback and the boot mstatus;
//   verify mstatus.MIE is clear, then install the trap handler
//   (direct-mode mtvec, mscratch scratch area).
//   Phase 1: write all-ones to mtvec and publish the readback
//   (MODE=3 is a reserved encoding, so the readback shows whether
//   the implementation drops the write); then write an all-ones
//   BASE with MODE=0 and publish that readback, which pins down
//   whether the drop was about MODE or about BASE.
//   Phase 2: write MODE=1 (vectored) at the handler address and
//   publish the readback; write MODE=0 (direct) at the handler
//   address and publish the readback. Together these pin down the
//   implemented modes by measurement.
//   Phase 3: write the boot mtvec value back, publish the
//   readback, verify it equals the boot value bit-for-bit; verify
//   mstatus is unchanged bit-for-bit.
//   Phase 4: the trap counter must be 0 (no interrupt enables are
//   set anywhere in the run, and MIE stays clear throughout).
//
// No interrupt can be taken: mstatus.MIE stays clear from boot to
// end (verified at boot and again at the end), and nothing in the
// run sets any interrupt enable. The trap handler is installed
// anyway and records mcause/mepc/mtval; any unexpected trap becomes
// visible instead of silent.
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

extern void mmw_trap_entry(void);

// Trap scratch: slot 0 trap count, slot 1 mcause, slot 2 mepc,
// slot 3 mtval, slot 4 parked t1. BSS-cleared to zero by boot.S.
unsigned long mmw_regs[5];

static unsigned long read_mtvec(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mtvec" : "=r"(v));
    return v;
}

static unsigned long read_mstatus(void) {
    unsigned long v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static void write_mtvec(unsigned long v) {
    __asm__ volatile("csrw mtvec, %0" : : "r"(v));
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
    unsigned long mtvec, mstatus;
    unsigned long boot_mtvec, boot_mstatus;
    unsigned long handler_base;
    unsigned long ones_rb, vec_rb, dir_rb, rest_rb;
    unsigned long vec_write, dir_write;

    uart_init();
    uart_puts("mtvec-mode-war: mtvec MODE WARL write/readback probe\n");

    // Global interrupt enable must be clear at boot: without MIE no
    // trap can be taken no matter what mtvec says.
    mstatus = read_mstatus();
    check((mstatus & MSTATUS_MIE) == 0, "mstatus.MIE set at boot");

    // Phase 0: record the boot baselines before touching anything.
    boot_mtvec = read_mtvec();
    boot_mstatus = read_mstatus();
    uart_puts("boot: mtvec=");
    uart_put_hex(boot_mtvec);
    uart_puts(" mstatus=");
    uart_put_hex(boot_mstatus);
    uart_puts("\n");
    digest64(boot_mtvec);
    digest64(boot_mstatus);

    // Install the trap handler: direct-mode mtvec, mscratch pointing
    // at the recording area.
    handler_base = (unsigned long)mmw_trap_entry & ~3UL;
    __asm__ volatile("csrw mtvec, %0" : : "r"(handler_base));
    __asm__ volatile("csrw mscratch, %0" : : "r"((unsigned long)mmw_regs));
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    uart_puts("trap: mtvec=");
    uart_put_hex(mtvec);
    uart_puts("\n");
    digest64(mtvec);
    check(mtvec == handler_base, "mtvec did not take the handler address");

    // Phase 1: all-ones write. MODE=3 is a reserved encoding, so
    // the only honest question is what the implementation does
    // with it: this QEMU drops the whole write. Then a second
    // write with an all-ones BASE and MODE=0 pins down whether the
    // drop was about MODE or about BASE: if the BASE sticks, the
    // drop was the reserved MODE encoding.
    write_mtvec(ALL_ONES);
    ones_rb = read_mtvec();
    uart_puts("phase1 ones: mtvec=");
    uart_put_hex(ones_rb);
    uart_puts("\n");
    digest64(ALL_ONES);
    digest64(ones_rb);
    check(ones_rb == mtvec,
          "phase 1: reserved-MODE write was not fully ignored");

    write_mtvec(ALL_ONES & ~3UL);
    ones_rb = read_mtvec();
    uart_puts("phase1 ones-base: mtvec=");
    uart_put_hex(ones_rb);
    uart_puts("\n");
    digest64(ALL_ONES & ~3UL);
    digest64(ones_rb);
    check(ones_rb == (ALL_ONES & ~3UL),
          "phase 1: all-ones BASE with MODE=0 did not stick");

    // Phase 2: MODE=1 (vectored) and MODE=0 (direct) writes at the
    // handler address, publishing each readback. The readback MODE
    // pins down which modes this implementation provides; the BASE
    // must come back intact either way.
    vec_write = handler_base | 1UL;
    write_mtvec(vec_write);
    vec_rb = read_mtvec();
    uart_puts("phase2 vectored: written=");
    uart_put_hex(vec_write);
    uart_puts(" readback=");
    uart_put_hex(vec_rb);
    uart_puts("\n");
    digest64(vec_write);
    digest64(vec_rb);
    check((vec_rb & 3UL) <= 1, "phase 2: vectored MODE read back reserved");
    check((vec_rb & ~3UL) == handler_base, "phase 2: vectored BASE changed");

    dir_write = handler_base | 0UL;
    write_mtvec(dir_write);
    dir_rb = read_mtvec();
    uart_puts("phase2 direct: written=");
    uart_put_hex(dir_write);
    uart_puts(" readback=");
    uart_put_hex(dir_rb);
    uart_puts("\n");
    digest64(dir_write);
    digest64(dir_rb);
    check((dir_rb & 3UL) <= 1, "phase 2: direct MODE read back reserved");
    check((dir_rb & ~3UL) == handler_base, "phase 2: direct BASE changed");

    // Phase 3: restore mtvec to the exact boot value and verify the
    // readback is bit-for-bit identical; mstatus must be unchanged
    // bit-for-bit as well.
    write_mtvec(boot_mtvec);
    rest_rb = read_mtvec();
    mstatus = read_mstatus();
    uart_puts("restored: mtvec=");
    uart_put_hex(rest_rb);
    uart_puts(" mstatus=");
    uart_put_hex(mstatus);
    uart_puts("\n");
    digest64(rest_rb);
    digest64(mstatus);
    check(rest_rb == boot_mtvec, "mtvec did not restore to the boot value");
    check(mstatus == boot_mstatus, "mstatus changed during run");

    // The trap handler must never have fired: MIE was clear for the
    // whole run and no enable was ever set.
    digest64(mmw_regs[0]);
    if (mmw_regs[0] != 0) {
        uart_puts("  FAIL: unexpected trap(s), count=");
        uart_put_dec(mmw_regs[0]);
        uart_puts(" mcause=");
        uart_put_hex(mmw_regs[1]);
        uart_puts(" mepc=");
        uart_put_hex(mmw_regs[2]);
        uart_puts(" mtval=");
        uart_put_hex(mmw_regs[3]);
        uart_puts("\n");
        checks++;
        mismatches++;
    }
    uart_puts("traps: count=");
    uart_put_dec(mmw_regs[0]);
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
