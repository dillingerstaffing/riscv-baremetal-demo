// mxr.h: shared definitions for the sstatus.MXR gate experiment
// (proof-backlog item "sstatus-mxr-probe").
//
// One mechanism under test: sstatus.MXR (bit 19) controls whether
// S-mode loads may read pages marked execute-only (privileged spec
// section 4.1.1.6: "when MXR=0, only loads from pages marked
// readable (R=1) will succeed; when MXR=1, loads from pages marked
// either readable or executable (R=1 or X=1) will succeed"). With
// MXR=0, a supervisor load from an X-only supervisor page must raise
// a load page fault; with MXR=1 the same load must succeed and
// return the page contents. This bit is distinct from the SUM gate
// tested in src/sstatus-sum (that one gates U-pages; this one gates
// X-only pages).
//
// Layout: src/boot.S jumps to main() in M-mode; the M-mode boot
// builds a minimal Sv39 table (identity megapages for code/data/UART,
// one 4 KiB X-only leaf mapping VA 0x40000000 to a canary page),
// opens the address space with one PMP NAPOT entry, delegates the
// load page fault (medeleg bit 13) to S-mode, installs the S-mode
// trap vector, enables Sv39 via satp, and mret drops into s_main()
// in S-mode. The S-mode payload runs two phases:
//   Phase A (MXR=0): one load from the X-only VA. Exactly one trap
//     must fire with scause 0xd (load page fault), stval = the
//     faulting VA, sepc = the faulting load; the handler advances
//     sepc by 4 and srets, and the poisoned t0 proves the load never
//     completed.
//   Phase B (MXR=1): csrs sstatus, MXR, then the same load must
//     return the canary with no new trap.
//
// The module shares only src/boot.S and the UART driver with the
// other demos via the Makefile; the boot, trap entry, handler, and
// reporting code here are written for this module alone.

#ifndef MXR_H
#define MXR_H

// Load page fault: scause exception code 13 (0xd).
#define SCAUSE_LPF 0xdUL

// sstatus.MXR is bit 19.
#define SSTATUS_MXR (1UL << 19)

// The one mapped execute-only virtual address: VPN[2]=1, VPN[1]=0,
// VPN[0]=0.
#define XPAGE_VA 0x40000000UL

// Canary word stored in the X-only page by M-mode before the drop.
#define CANARY 0xE4EC0D1EDEADBEEFUL

// Identity-mapped window for the S-mode phase (1 GiB megapage at
// root[2] covers [0x80000000, 0xC0000000); a second 1 GiB leaf at
// root[0] covers [0, 0x40000000), which includes the UART at
// 0x10000000).
#define IDENT_BASE 0x80000000UL
#define IDENT_END  0xC0000000UL
#define UART_BASE  0x10000000UL

// PTE flag bits (RISC-V privileged spec, Sv39 PTE format).
#define PTE_V 0x001UL
#define PTE_R 0x002UL
#define PTE_W 0x004UL
#define PTE_X 0x008UL
#define PTE_U 0x010UL
#define PTE_G 0x020UL
#define PTE_A 0x040UL
#define PTE_D 0x080UL

#define MSTATUS_MPP_MASK (3UL << 11)
#define MSTATUS_MPP_S (1UL << 11)
#define SATP_MODE_SV39 (8UL << 60)

// medeleg bit 13: delegate load page faults to S-mode.
#define MEDELEG_LPF (1UL << 13)

// Trap save area. Offsets must match mxr_trap.S:
//   t0_save@0 t1_save@8 scause@16 sepc@24 stval@32,
//   then ra,sp,gp,tp,t2,s0,s1,a0-a7,s2-s11,t3-t6 at 48,56,...,272.
typedef struct {
    unsigned long t0_save;
    unsigned long t1_save;
    unsigned long scause;
    unsigned long sepc;
    unsigned long stval;
    unsigned long gpr[29];
} mxr_save_t;

extern mxr_save_t mxr_save;
extern volatile unsigned long mxr_trap_count;
extern volatile unsigned long mxr_scause;      // the one trap's scause
extern volatile unsigned long mxr_stval;       // the one trap's stval
extern volatile unsigned long mxr_sepc;        // the one trap's sepc
extern volatile unsigned long mxr_bad_seen;   // any trap beyond the first
extern volatile unsigned long mxr_bad_scause;
extern volatile unsigned long mxr_bad_sepc;

// Label on the phase-A faulting load inside the asm block
// (mxr_main.c): the one trap's sepc must equal this address.
extern char mxrs_fault_load[];

void mxr_trap_entry(void);
void mxr_trap_handler(mxr_save_t *s);

// S-mode entry point: M-mode boot finishes with mret to s_main.
void s_main(void);

#endif // MXR_H
