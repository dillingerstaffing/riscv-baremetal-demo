# Makefile for riscv-baremetal-demo.
# Requires: riscv64-unknown-elf-gcc and qemu-system-riscv64
# (Debian/Ubuntu: gcc-riscv64-unknown-elf and qemu-system-misc).

CROSS   ?= riscv64-unknown-elf-
CC      := $(CROSS)gcc

CFLAGS  := -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
           -no-pie -fno-pie -fno-pic \
           -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany
LDFLAGS := -T link.ld

SRCS := src/boot.S src/switch.S src/uart.c src/sched.c src/tasks.c src/main.c
OBJS := $(SRCS:.c=.o)
OBJS := $(OBJS:.S=.o)

# Misaligned-access experiment module: its own binary sharing only boot.S
# and the UART driver with the other demos. Issues a misaligned lw and a
# misaligned sw at fixed unaligned addresses and reports whether each
# traps (with the exact trap register values) or completes transparently.
MIS_SRCS := src/boot.S src/uart.c \
            src/misaligned/mis_trap.S src/misaligned/mis_main.c
MIS_OBJS := $(MIS_SRCS:.c=.o)
MIS_OBJS := $(MIS_OBJS:.S=.o)

mal.elf: $(MIS_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MIS_OBJS)

# Misaligned LR/SC experiment module: its own binary sharing only
# boot.S and the UART driver with the other demos. Issues a misaligned
# lr.w, a misaligned sc.w after an aligned lr, and a misaligned lr/sc
# pair at fixed unaligned addresses and reports whether each traps
# (with the exact trap register values) or completes transparently.
AMO_SRCS := src/boot.S src/uart.c \
            src/amo/amo_trap.S src/amo/amo_main.c
AMO_OBJS := $(AMO_SRCS:.c=.o)
AMO_OBJS := $(AMO_OBJS:.S=.o)

amo.elf: $(AMO_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(AMO_OBJS)

# amoadd.w read-modify-write module: its own binary sharing only boot.S
# and the UART driver with the other demos. Runs 10000 amoadd.w +1 and
# 100 amoadd.w +7 on a single hart, checking every returned old value
# against the expected sequence, the exact final values, and the
# arithmetic-series sums of the returned streams. A minimal trap handler
# counts traps and parks the hart on the first one, so a printed PASS
# implies zero traps. Single-hart only: this verifies the instruction's
# read-modify-write contract, not atomicity under multi-hart contention.
AAA_SRCS := src/boot.S src/uart.c \
            src/amo-add-atomicity/aaa_trap.S src/amo-add-atomicity/aaa_main.c
AAA_OBJS := $(AAA_SRCS:.c=.o)
AAA_OBJS := $(AAA_OBJS:.S=.o)

amo-add-atomicity.elf: $(AAA_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(AAA_OBJS)

# LR/SC attempt-histogram module: its own binary sharing only boot.S
# and the UART driver with the other demos. Runs 10,000 aligned
# lr.w/sc.w pairs on a single hart with no contention, records the
# attempts-to-success histogram, and verifies every stored value by
# readback. A minimal trap handler halts the hart on any trap, so the
# run's own PASS/FAIL verdict also covers "no trap fired".
LRH_SRCS := src/boot.S src/uart.c \
            src/lrsc-histogram/lrsc_trap.S src/lrsc-histogram/lrsc_main.c
LRH_OBJS := $(LRH_SRCS:.c=.o)
LRH_OBJS := $(LRH_OBJS:.S=.o)

lrsc-histogram.elf: $(LRH_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LRH_OBJS)

# Run the LR/SC attempt-histogram module under QEMU.
run-lrsc-histogram: lrsc-histogram.elf
	$(QEMU) -machine virt -nographic -bios none -kernel lrsc-histogram.elf

# SC-without-reservation failure-path module: its own binary sharing
# only boot.S and the UART driver with the other demos. Issues sc.w
# with no preceding lr.w, then lr.w on address A followed by sc.w on
# address B, and reports for each the rd result, the before/after
# memory readbacks, and the trap count (0 expected). A minimal trap
# handler records mcause/mepc/mtval and halts the hart on any trap.
SCF_SRCS := src/boot.S src/uart.c \
            src/sc-fail/scf_trap.S src/sc-fail/scf_main.c
SCF_OBJS := $(SCF_SRCS:.c=.o)
SCF_OBJS := $(SCF_OBJS:.S=.o)

sc-fail.elf: $(SCF_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SCF_OBJS)

# Run the SC failure-path module under QEMU.
run-sc-fail: sc-fail.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sc-fail.elf

# mret-no-restore module: its own binary sharing only boot.S and the
# UART driver with the other demos. Loads eight distinct sentinel
# values into a0..a7, records them, and issues exactly one ecall; the
# M-mode handler records mcause/mepc/mtval, writes a different set of
# eight sentinels into a0..a7, advances mepc past the ecall, and mrets
# with no register restore. The pre-trap code reads a0..a7 back and
# asserts all eight changed and match the handler's values. On PASS it
# shuts the machine down via the virt test-device finisher so the
# QEMU process exit code (0) reflects the verdict; on FAIL it parks
# the hart instead.
MNR_SRCS := src/boot.S src/uart.c \
            src/mret-no-restore/mnr_trap.S src/mret-no-restore/mnr_main.c
MNR_OBJS := $(MNR_SRCS:.c=.o)
MNR_OBJS := $(MNR_OBJS:.S=.o)

mret-no-restore.elf: $(MNR_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MNR_OBJS)

# Run the mret-no-restore module under QEMU.
run-mret-no-restore: mret-no-restore.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mret-no-restore.elf

# mepc-resume-skip module: its own binary sharing only boot.S and the
# UART driver with the other demos. Executes one 4-byte faulting load
# (lw a0, 0(a1) at an unmapped address, mcause=5); the M-mode handler
# records mcause/mtval/mepc-at-entry, adds 4 to mepc, records the
# adjusted mepc, and mrets. The instruction at faulting+4 writes a
# marker word, and the program asserts the trap fired exactly once,
# the mepc delta is 4, the marker ran, and the faulting load never
# committed (a0 keeps its pre-fault sentinel). On PASS it shuts the
# machine down via the virt test-device finisher so the QEMU process
# exit code (0) reflects the verdict; on FAIL it parks the hart
# instead.
MRS_SRCS := src/boot.S src/uart.c \
            src/mepc-resume-skip/mrs_trap.S src/mepc-resume-skip/mrs_main.c
MRS_OBJS := $(MRS_SRCS:.c=.o)
MRS_OBJS := $(MRS_OBJS:.S=.o)

mepc-resume-skip.elf: $(MRS_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MRS_OBJS)

# Run the mepc-resume-skip module under QEMU.
run-mepc-resume-skip: mepc-resume-skip.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mepc-resume-skip.elf

# sepc-resume-skip module: its own binary sharing only boot.S and the
# UART driver with the other demos. M-mode boot code delegates the
# supervisor load access fault to S-mode via medeleg bit 5, installs
# a direct-mode stvec handler, opens the address space with one PMP
# NAPOT R/W/X entry, then mrets into an S-mode payload that executes
# one 4-byte faulting load (lw a0, 0(a1) at an unmapped address,
# scause=5); the S-mode handler records scause/stval/sepc-at-entry,
# adds 4 to sepc, records the adjusted sepc, and srets. The
# instruction at faulting+4 writes a marker word, and the program
# asserts the trap fired exactly once, the sepc delta is 4, the marker
# ran, and the faulting load never committed (a0 keeps its pre-fault
# sentinel). On PASS it shuts the machine down via the virt
# test-device finisher so the QEMU process exit code (0) reflects the
# verdict; on FAIL it parks the hart instead.
SS_SRCS := src/boot.S src/uart.c \
           src/sepc-skip/ss_trap.S src/sepc-skip/ss_main.c
SS_OBJS := $(SS_SRCS:.c=.o)
SS_OBJS := $(SS_OBJS:.S=.o)

sepc-resume-skip.elf: $(SS_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SS_OBJS)

# Run the sepc-resume-skip module under QEMU.
run-sepc-resume-skip: sepc-resume-skip.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sepc-resume-skip.elf

# stvec direct-mode module: its own binary sharing only boot.S and the
# UART driver with the other demos. Writes stvec with MODE=00
# (direct), reads it back to verify the mode bits read back clear and
# the base matches, delegates the S-mode environment call via
# medeleg bit 9, then mretes with mstatus.MPP=01 into an S-mode
# payload that is a single ecall at an address captured with an in-asm
# numeric local label. The S-mode handler at the stvec base records
# scause/sepc/stval, prints the trap record, and prints RESULT: PASS
# only if exactly 1 trap fired with scause == 9, sepc == the captured
# ecall address, and stval == 0. On PASS it shuts the machine down
# via the virt test-device finisher so the QEMU process exit code (0)
# reflects the verdict; on FAIL it parks the hart instead.
SVD_SRCS := src/boot.S src/uart.c \
            src/stvec-direct/svd_trap.S src/stvec-direct/svd_main.c
SVD_OBJS := $(SVD_SRCS:.c=.o)
SVD_OBJS := $(SVD_OBJS:.S=.o)

stvec-direct.elf: $(SVD_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SVD_OBJS)

# Run the stvec direct-mode module under QEMU.
run-stvec-direct: stvec-direct.elf
	$(QEMU) -machine virt -nographic -bios none -kernel stvec-direct.elf

# satp-asid module: its own binary sharing only boot.S and the UART
# driver with the other demos. Drops to S-mode via mret (MPP=01) and
# runs the satp experiment there: WARL discovery of the ASID field
# width by writing all-ones to ASID with MODE=0/PPN=0, write/readback
# round-trips over ASID {0, 1, mid, max-writable} with MODE and PPN
# held at zero, and a MODE WARL probe (write reserved encoding 15,
# expect a legal 0-8 readback) that never enables translation. An
# M-mode trap handler records and parks on any trap; reaching the
# completion marker with the trap counter at 0 is the no-trap proof.
# On PASS it shuts the machine down via the virt test-device
# finisher so the QEMU process exit code (0) reflects the verdict.
SATPA_SRCS := src/boot.S src/uart.c \
              src/satp-asid/satp_trap.S src/satp-asid/satp_main.c
SATPA_OBJS := $(SATPA_SRCS:.c=.o)
SATPA_OBJS := $(SATPA_OBJS:.S=.o)

satp-asid.elf: $(SATPA_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SATPA_OBJS)

# Run the satp-asid module under QEMU.
run-satp-asid: satp-asid.elf
	$(QEMU) -machine virt -nographic -bios none -kernel satp-asid.elf

# satp-bare module: its own binary sharing only boot.S and the UART
# driver with the other demos. Drops to S-mode via mret (MPP=01) and
# runs the Bare-mode experiment there: csrw satp with MODE=0 (Bare)
# and a nonzero PPN, csrr readback (MODE field must read 0), then an
# S-mode load of a scratch physical word must return the canary
# M-mode stored there, and an S-mode store/loadback must round-trip,
# which is exactly "Bare means the effective address is the physical
# address". Had translation been active, the load would have walked
# the garbage PPN as a page-table root and faulted. An M-mode trap
# handler records and parks on any trap; reaching the completion
# marker with the trap counter at 0 is the no-trap proof. On PASS it
# shuts the machine down via the virt test-device finisher so the
# QEMU process exit code (0) reflects the verdict.
SATPB_SRCS := src/boot.S src/uart.c \
              src/satp-bare/bare_trap.S src/satp-bare/bare_main.c
SATPB_OBJS := $(SATPB_SRCS:.c=.o)
SATPB_OBJS := $(SATPB_OBJS:.S=.o)

satp-bare.elf: $(SATPB_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SATPB_OBJS)

# Run the satp-bare module under QEMU.
run-satp-bare: satp-bare.elf
	$(QEMU) -machine virt -nographic -bios none -kernel satp-bare.elf

# satp MODE WARL probe module: its own binary sharing only boot.S and
# the UART driver with the other demos. Probes the satp.MODE field in
# M-mode across Bare (0), Sv39 (8), Sv48 (9), Sv57 (10), and reserved
# encoding 15, publishing every write/readback pair; M-mode is used
# because a MODE write that sticks cannot fault anything there
# (M-mode accesses are never translated), so the probe is safe even
# when a write takes effect. The probe accepts either a clean stick
# (readback == write) or clean no-effect (readback == prior) for
# 9/10/15 so it documents rather than assumes the hart's supported
# modes; on QEMU 8.2.2 virt, 9 and 10 stick and 15 has no effect.
# After discovery it restores satp and drops to S-mode via mret
# (MPP=01), where the Sv39 write uses a hand-verified two-leaf page
# table (1 GiB megapage leaves: root[2] identity-maps
# [0x80000000, 0xC0000000) R/W/X, root[0] identity-maps [0,
# 0x40000000) R/W covering the UART); after the MODE=8 readback a RAM
# canary stored with translation off is read through the active
# translation to prove the walk resolved to the intended frame, then
# satp returns to Bare. An M-mode trap handler records and parks on
# any trap; reaching the completion marker with the trap counter at
# 0 is the no-trap proof. A checks/mismatches summary and an FNV-1a
# digest of every verdict-relevant value are printed for run-to-run
# comparison.
# On PASS it shuts the machine down via the virt test-device
# finisher so the QEMU process exit code (0) reflects the verdict;
# on FAIL it parks the hart instead.
SMW_SRCS := src/boot.S src/uart.c \
            src/satp-mode-warl/warl_trap.S src/satp-mode-warl/warl_main.c
SMW_OBJS := $(SMW_SRCS:.c=.o)
SMW_OBJS := $(SMW_OBJS:.S=.o)

satp-mode-warl.elf: $(SMW_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SMW_OBJS)

# Run the satp MODE WARL probe module under QEMU.
run-satp-mode-warl: satp-mode-warl.elf
	$(QEMU) -machine virt -nographic -bios none -kernel satp-mode-warl.elf
# sip WARL write/readback probe module: its own binary sharing only
# boot.S and the UART driver with the other demos. Records the boot
# sip/mip/mideleg baselines, writes all-ones to sip and reads back
# the legalized value (must equal the boot baseline while SSI is not
# delegated), writes zero and confirms the baseline readback, then
# delegates SSI in mideleg and repeats: the all-ones write must
# legalize to exactly SSIP (bit 1) set with every other bit unchanged,
# mip bit 1 must follow the sip writes as the read-only alias with
# all other mip bits unchanged, and the zero write must return sip
# byte-identical to the boot baseline. A trap handler recording
# mcause/mepc/mtval is installed but must never fire (trap count 0),
# with mie.MSIE and mstatus.MIE read back clear at boot and at the
# end. Prints a checks/mismatches summary and an FNV-1a digest of
# the verdict-relevant values for run-to-run comparison. On PASS it
# shuts the machine down via the virt test-device finisher so the
# QEMU process exit code (0) reflects the verdict; on FAIL it parks
# the hart instead.
SIPW_SRCS := src/boot.S src/uart.c \
             src/sip-write-probe/swp_trap.S src/sip-write-probe/swp_main.c
SIPW_OBJS := $(SIPW_SRCS:.c=.o)
SIPW_OBJS := $(SIPW_OBJS:.S=.o)

sip-write-probe.elf: $(SIPW_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SIPW_OBJS)

# Run the sip WARL write/readback probe module under QEMU.
run-sip-write-probe: sip-write-probe.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sip-write-probe.elf

# M-mode to U-mode trap transition module: its own binary sharing only
# boot.S and the UART driver with the other demos. Drops to U-mode with
# mret (mstatus.MPP = 0) into a one-instruction ecall payload; the
# M-mode handler records mcause/mepc/mtval and the entry mstatus, then
# returns to M-mode. The verdict checks mcause == 8 (ecall from
# U-mode), mepc == the payload ecall address, and entry MPP == U.
UMODE_SRCS := src/boot.S src/uart.c \
              src/umode/umode_trap.S src/umode/umode_main.c
UMODE_OBJS := $(UMODE_SRCS:.c=.o)
UMODE_OBJS := $(UMODE_OBJS:.S=.o)

umode.elf: $(UMODE_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(UMODE_OBJS)

# medeleg writable-mask module: its own binary sharing only boot.S and
# the UART driver with the other demos. Records the boot-time medeleg
# and mideleg values, writes all-ones to each CSR and reads back the
# writable mask (two write/read cycles must agree), restores both to
# their boot values, and checks an M-mode ecall trap taken before the
# writes reports identical mcause/mepc/mtval to one taken after the
# restore.
MDEL_SRCS := src/boot.S src/uart.c \
             src/medeleg-mask/mdel_trap.S src/medeleg-mask/mdel_main.c
MDEL_OBJS := $(MDEL_SRCS:.c=.o)
MDEL_OBJS := $(MDEL_OBJS:.S=.o)

medeleg-mask.elf: $(MDEL_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MDEL_OBJS)

# Run the medeleg writable-mask module under QEMU.
run-medeleg-mask: medeleg-mask.elf
	$(QEMU) -machine virt -nographic -bios none -kernel medeleg-mask.elf

# mideleg selective-routing module: its own binary sharing only boot.S
# and the UART driver with the other demos. Sets mideleg to delegate
# only the supervisor external interrupt (bit 9, readback-verified),
# then fires a supervisor timer interrupt (pended via mip.STIP, not
# delegated, must trap to M-mode) and a PLIC supervisor-context UART
# interrupt (must trap to S-mode), publishing mcause/scause and the
# delegation readbacks.
MIDR_SRCS := src/boot.S src/uart.c \
             src/mideleg-route/midr_trap.S src/mideleg-route/midr_main.c
MIDR_OBJS := $(MIDR_SRCS:.c=.o)
MIDR_OBJS := $(MIDR_OBJS:.S=.o)

mideleg-route.elf: $(MIDR_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MIDR_OBJS)

# Run the mideleg selective-routing module under QEMU.
run-mideleg-route: mideleg-route.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mideleg-route.elf

# stvec-vectored module: its own binary sharing only boot.S and the
# UART driver with the other demos. Installs stvec in vectored mode
# (MODE=1) over a 16-entry table of single 4-byte jal stubs in S-mode,
# delegates the supervisor timer interrupt (mideleg bit 5) and the
# supervisor external interrupt (mideleg bit 9) to S-mode, then raises
# both and checks each trap landed at BASE + 4*code with the right
# scause: timer (code 5) at BASE + 20, external (code 9) at BASE + 36,
# each exactly once, no unexpected traps. On PASS it shuts the machine
# down via the virt test-device finisher so the QEMU process exit code
# (0) reflects the verdict; on FAIL it parks the hart instead.
SVV_SRCS := src/boot.S src/uart.c \
            src/stvec-vectored/stvv_trap.S src/stvec-vectored/stvv_main.c
SVV_OBJS := $(SVV_SRCS:.c=.o)
SVV_OBJS := $(SVV_OBJS:.S=.o)

stvec-vectored.elf: $(SVV_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SVV_OBJS)

# Run the stvec vectored-offset module under QEMU.
run-stvec-vectored: stvec-vectored.elf
	$(QEMU) -machine virt -nographic -bios none -kernel stvec-vectored.elf

# stvec-vectored-mode module: its own binary sharing only boot.S and
# the UART driver with the other demos. Installs stvec in vectored
# mode (MODE=1) over a two-slot table of single 4-byte jal stubs in
# S-mode, delegates only the illegal-instruction exception (medeleg
# bit 2) and only the supervisor software interrupt (mideleg bit 1),
# then measures the true vectored-mode split: the exception must
# enter at BASE (slot 0) with scause = 0x2 and sepc at the illegal
# word, while the pending SSI must enter at BASE+4 (slot 1) with
# scause = 0x8000000000000001 and sepc at the interrupted nop. Each
# stub records its own hardware entry pc (BASE + 4*cause) into the
# trap log, so the landing address is measured. On PASS it parks the
# hart via wfi so the QEMU process is stopped with the output
# captured; on FAIL it parks the same way.
STVM_SRCS := src/boot.S src/uart.c \
            src/stvec-vectored-mode/stvm_trap.S src/stvec-vectored-mode/stvm_main.c
STVM_OBJS := $(STVM_SRCS:.c=.o)
STVM_OBJS := $(STVM_OBJS:.S=.o)

stvec-vectored-mode.elf: $(STVM_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(STVM_OBJS)

# Run the stvec-vectored-mode module under QEMU.
run-stvec-vectored-mode: stvec-vectored-mode.elf
	$(QEMU) -machine virt -nographic -bios none -kernel stvec-vectored-mode.elf

# mtval-fault-address module: its own binary sharing only boot.S and
# the UART driver with the other demos. Issues a load access fault and
# a store access fault at the same unmapped address and publishes the
# mcause/mepc/mtval triple for each, verifying both traps report the
# exact faulting address in mtval (mcause 0x5 vs 0x7). The fault and
# resume addresses are captured with in-asm numeric local labels; the
# handler records mcause/mtval/mepc per trap into a two-record save
# area and resumes at a resume address each test stored before the
# fault. On PASS it shuts the machine down via the virt test-device
# finisher so the QEMU process exit code (0) reflects the verdict; on
# FAIL it parks the hart instead.
MFA_SRCS := src/boot.S src/uart.c \
            src/mtval-fault-address/mfa_trap.S src/mtval-fault-address/mfa_main.c
MFA_OBJS := $(MFA_SRCS:.c=.o)
MFA_OBJS := $(MFA_OBJS:.S=.o)

mtval-fault-address.elf: $(MFA_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MFA_OBJS)

# Run the mtval-fault-address module under QEMU.
run-mtval-fault-address: mtval-fault-address.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mtval-fault-address.elf

# mcounteren module: its own binary sharing only boot.S and the UART
# driver with the other demos. Writes mcounteren = 0, verifies the
# readback is 0x0, then proves rdcycle still advances in M-mode
# (mcounteren gates only S and U mode reads), and finally restores
# mcounteren to the boot value and verifies the readback. On PASS
# it shuts the machine down via the virt test-device finisher so
# the QEMU process exit code (0) reflects the verdict; on FAIL it
# parks the hart instead.
MCE_SRCS := src/boot.S src/uart.c \
            src/mcounteren/mce_main.c
MCE_OBJS := $(MCE_SRCS:.c=.o)
MCE_OBJS := $(MCE_OBJS:.S=.o)

mcounteren.elf: $(MCE_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MCE_OBJS)

# Run the mcounteren module under QEMU.
run-mcounteren: mcounteren.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mcounteren.elf

all: demo.elf preempt.elf virtio-blk.elf smp.elf shell.elf uart-baud.elf smode.elf smode-mbase.elf pmp.elf wfi-latency.elf mal.elf plic.elf mtimecmp.elf sv39.elf csr.elf ecall.elf counter-alias.elf amo.elf umode.elf msip.elf mtvec-vectored.elf cycmon.elf fs-check.elf medeleg-mask.elf wfi-resume-pc.elf lrsc-histogram.elf pmp-tor.elf mpp-encoding.elf mcycle-write.elf mip-msip.elf mie-global.elf sip-ssip.elf sc-fail.elf mret-no-restore.elf stvec-direct.elf mepc-resume-skip.elf sepc-resume-skip.elf pmp-napot-size.elf satp-asid.elf satp-bare.elf mtval-fault-address.elf mcounteren.elf cycle-read-latency.elf mtimecmp-oneshot.elf stimecmp-one-shot.elf mie-stie.elf mcause-warl.elf mideleg-route.elf sepc-warl.elf scause-bit.elf mideleg-warl.elf mtvec-mode0-direct.elf pmp-lock-bit.elf mie-toggle.elf mscratch-csrrw.elf sstatus-spp.elf sip-write-probe.elf mip-pending-no-trap.elf sstatus-sie-gate.elf sip-stip-write.elf mcause-interrupt-bit.elf scause-warl.elf sstatus-sum.elf sie-stie-gate.elf sie-stie-write.elf mip-msip-write.elf mstatus-sie-toggle.elf sstatus-mxr.elf amo-add-atomicity.elf sip-seip-write.elf scounteren-ir-gate.elf mideleg-ssip-route.elf medeleg-ecall-destination.elf medeleg-ecall-u-route.elf mideleg-mtip-route.elf mie-msie-gate.elf mie-mtie-gate.elf mideleg-seip-route.elf sip-stip-mideleg-reconcile.elf medeleg-illegal-inst-route.elf medeleg-breakpoint.elf stval-illegal-capture.elf stval-ecall-capture.elf pmp-napot-encode.elf sstatus-fs-dirty.elf satp-mode-warl.elf sstatus-spp-sret-u.elf sie-ssip-clear-suppresses.elf stvec-vectored-mode.elf fflags-nx-inexact.elf frm-rounding-write.elf fflags-uf-underflow.elf fflags-of-overflow.elf fflags-dz-divide-by-zero.elf fflags-nv-invalid.elf mtimecmp-delta-tracks-mtime.elf mtimecmp-rw.elf fcsr-field-independence.elf fcsr-frm-roundup.elf fcsr-frm-rounddn.elf

demo.elf: $(OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

# Preemptive-scheduler module: its own binary sharing only boot.S and the
# UART driver with the cooperative demo.
PREEMPT_SRCS := src/boot.S src/uart.c \
                src/preempt/trap.S src/preempt/clint.c src/preempt/psched.c \
                src/preempt/ptasks.c src/preempt/pmain.c
PREEMPT_OBJS := $(PREEMPT_SRCS:.c=.o)
PREEMPT_OBJS := $(PREEMPT_OBJS:.S=.o)

preempt.elf: $(PREEMPT_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PREEMPT_OBJS)

# virtio-blk module: its own binary sharing only boot.S and the
# UART driver with the other demos. Needs a raw disk image at run time
# (see the disk.img rule and run-virtio below).
VIRTIO_SRCS := src/boot.S src/uart.c \
               src/virtio-blk/virtio.c src/virtio-blk/blk.c src/virtio-blk/bmain.c
VIRTIO_OBJS := $(VIRTIO_SRCS:.c=.o)
VIRTIO_OBJS := $(VIRTIO_OBJS:.S=.o)

virtio-blk.elf: $(VIRTIO_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(VIRTIO_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

# Run under QEMU. -bios none skips firmware so we boot straight into _start
# in M-mode; -nographic wires the virt UART to the terminal.
# QEMU is overridable: make run QEMU=path/to/qemu-system-riscv64
QEMU ?= qemu-system-riscv64
run: demo.elf
	$(QEMU) -machine virt -nographic -bios none -kernel demo.elf

# Run the preemptive-scheduler module under QEMU.
run-preempt: preempt.elf
	$(QEMU) -machine virt -nographic -bios none -kernel preempt.elf

# 16 MiB raw disk image backing the virtio-blk device (not committed).
disk.img:
	dd if=/dev/zero of=$@ bs=1M count=16

# Run the virtio-blk module under QEMU, with the raw image attached as a
# virtio-blk-device (QEMU maps it to the next free virtio-mmio slot).
run-virtio: virtio-blk.elf disk.img
	$(QEMU) -machine virt -nographic -bios none -kernel virtio-blk.elf \
		-drive file=disk.img,if=none,format=raw,id=hd0 \
		-device virtio-blk-device,drive=hd0

# SMP bring-up module: its own binary sharing only the UART driver with
# the other demos. Boots two harts (-smp 2).
SMP_SRCS := src/smp/smp_boot.S src/uart.c \
            src/smp/spinlock.c src/smp/smp_print.c \
            src/smp/smp_main.c src/smp/hart1.c
SMP_OBJS := $(SMP_SRCS:.c=.o)
SMP_OBJS := $(SMP_OBJS:.S=.o)

smp.elf: $(SMP_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SMP_OBJS)

# Run the SMP bring-up module under QEMU with two harts.
run-smp: smp.elf
	$(QEMU) -machine virt -nographic -bios none -smp 2 -kernel smp.elf

# UART shell module: its own binary sharing only the UART driver with
# the other demos. Interactive over the virt serial port (use
# run-shell-piped to feed it scripted input).
SHELL_SRCS := src/boot.S src/uart.c \
              src/shell/trap.S src/shell/sh_main.c src/shell/cmds.c
SHELL_OBJS := $(SHELL_SRCS:.c=.o)
SHELL_OBJS := $(SHELL_OBJS:.S=.o)

shell.elf: $(SHELL_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SHELL_OBJS)

run-shell: shell.elf
	$(QEMU) -machine virt -nographic -bios none -kernel shell.elf

# UART baud-timing module: its own binary sharing only boot.S and the
# UART driver with the other demos. Programs the divisor latch and
# measures the resulting bit timing via the FIFO receive timeout.
UARTBAUD_SRCS := src/boot.S src/uart.c src/uart-baud/baud_main.c
UARTBAUD_OBJS := $(UARTBAUD_SRCS:.c=.o)
UARTBAUD_OBJS := $(UARTBAUD_OBJS:.S=.o)

uart-baud.elf: $(UARTBAUD_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(UARTBAUD_OBJS)

# Run the UART baud-timing module under QEMU.
run-uart-baud: uart-baud.elf
	$(QEMU) -machine virt -nographic -bios none -kernel uart-baud.elf

# S-mode trap delegation module: two binaries sharing the scheduler core,
# compiled once per privilege mode via -DTRAP_SMODE.
#   smode.elf: M-mode boot programs medeleg/mideleg, installs stvec, drops
#     to S-mode with sret; the scheduler runs on the delegated supervisor
#     timer interrupt (stimecmp when Sstc is present, M-mode ecall rearm
#     otherwise).
#   smode-mbase.elf: the identical workload in M-mode on the machine timer
#     interrupt; the baseline the delegation overhead is measured against.
SMODE_S_OBJS := src/smode/sboot_s.o src/smode/msetup_s.o src/smode/strap_s.o \
                src/smode/ssched_s.o src/smode/stimer_s.o src/smode/smain_s.o \
                src/smode/stasks_s.o src/uart.o
SMODE_M_OBJS := src/smode/sboot_m.o src/smode/msetup_m.o src/smode/strap_m.o \
                src/smode/ssched_m.o src/smode/stimer_m.o src/smode/smain_m.o \
                src/smode/stasks_m.o src/uart.o

src/smode/%_s.o: src/smode/%.c
	$(CC) $(CFLAGS) -DTRAP_SMODE=1 -c $< -o $@
src/smode/%_s.o: src/smode/%.S
	$(CC) $(CFLAGS) -DTRAP_SMODE=1 -c $< -o $@
src/smode/%_m.o: src/smode/%.c
	$(CC) $(CFLAGS) -DTRAP_SMODE=0 -c $< -o $@
src/smode/%_m.o: src/smode/%.S
	$(CC) $(CFLAGS) -DTRAP_SMODE=0 -c $< -o $@

smode.elf: $(SMODE_S_OBJS) link.ld
	$(CC) $(CFLAGS) -DTRAP_SMODE=1 $(LDFLAGS) -o $@ $(SMODE_S_OBJS)

smode-mbase.elf: $(SMODE_M_OBJS) link.ld
	$(CC) $(CFLAGS) -DTRAP_SMODE=0 $(LDFLAGS) -o $@ $(SMODE_M_OBJS)

# Run the S-mode delegation module and the M-mode baseline under QEMU.
run-smode: smode.elf
	$(QEMU) -machine virt -nographic -bios none -kernel smode.elf

run-smode-mbase: smode-mbase.elf
	$(QEMU) -machine virt -nographic -bios none -kernel smode-mbase.elf

# PMP denial-test module: its own binary sharing only boot.S and the
# UART driver with the other demos. Programs a locked no-access PMP
# region and verifies the load/store access-fault trap codes.
PMP_SRCS := src/boot.S src/uart.c \
            src/pmp/pmp_trap.S src/pmp/pmp_main.c
PMP_OBJS := $(PMP_SRCS:.c=.o)
PMP_OBJS := $(PMP_OBJS:.S=.o)

pmp.elf: $(PMP_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PMP_OBJS)

# Run the PMP denial-test module under QEMU.
run-pmp: pmp.elf
	$(QEMU) -machine virt -nographic -bios none -kernel pmp.elf

# PMP lock-bit persistence module: its own binary sharing only boot.S
# and the UART driver with the other demos. Programs a locked no-access
# PMP entry and verifies the lock-bit semantics the privileged spec
# assigns: all-ones writes to pmpcfg0 and pmpaddr0 are ignored while the
# entry's L bit is set (readbacks unchanged), and the locked entry's
# permissions are enforced even in M-mode (the load traps with a load
# access fault, mcause 5).
PLB_SRCS := src/boot.S src/uart.c \
            src/pmp-lock-bit/plb_trap.S src/pmp-lock-bit/plb_main.c
PLB_OBJS := $(PLB_SRCS:.c=.o)
PLB_OBJS := $(PLB_OBJS:.S=.o)

pmp-lock-bit.elf: $(PLB_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PLB_OBJS)

# Run the PMP lock-bit persistence module under QEMU.
run-pmp-lock-bit: pmp-lock-bit.elf
	$(QEMU) -machine virt -nographic -bios none -kernel pmp-lock-bit.elf

# mstatus.MIE gate module: its own binary sharing only boot.S and the
# UART driver with the other demos. Pends a CLINT machine timer
# interrupt with mstatus.MIE clear (no trap may fire while mip.MTIP
# reads 1), then sets MIE and verifies exactly one trap with mcause
# 0x8000000000000007, the handler disarms mtimecmp, and the trap count
# stays at 1 through a quiet window.
METOG_SRCS := src/boot.S src/uart.c \
              src/mie-toggle/mie-toggle_trap.S src/mie-toggle/mie-toggle_main.c
METOG_OBJS := $(METOG_SRCS:.c=.o)
METOG_OBJS := $(METOG_OBJS:.S=.o)

mie-toggle.elf: $(METOG_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(METOG_OBJS)

# Run the mstatus.MIE gate module under QEMU.
run-mie-toggle: mie-toggle.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mie-toggle.elf

# PMP TOR-boundary module: its own binary sharing only boot.S and the
# UART driver with the other demos. Two TOR entries form one exact
# boundary: the last allowed byte reads clean, the first denied byte
# traps with a load access fault (mcause 5).
PMPTOR_SRCS := src/boot.S src/uart.c \
               src/pmp-tor/pmt_trap.S src/pmp-tor/pmt_main.c
PMPTOR_OBJS := $(PMPTOR_SRCS:.c=.o)
PMPTOR_OBJS := $(PMPTOR_OBJS:.S=.o)

pmp-tor.elf: $(PMPTOR_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PMPTOR_OBJS)

# Run the PMP TOR-boundary module under QEMU.
run-pmp-tor: pmp-tor.elf
	$(QEMU) -machine virt -nographic -bios none -kernel pmp-tor.elf

# PMP NAPOT size-decoding module: its own binary sharing only boot.S and
# the UART driver with the other demos. Programs a locked no-access
# NAPOT entry at 4 KiB and a second locked no-access NAPOT entry at
# 64 KiB over the same 64 KiB scratch region, then proves by boundary
# probes (last byte inside / first byte outside for each encoded size,
# plus the same address probed under both encodings) that the
# fault/no-fault boundary moves exactly as the pmpaddr trailing-ones
# size encoding predicts. On PASS it parks the hart; capture the log
# with timeout.
PNS_SRCS := src/boot.S src/uart.c \
            src/pmp-napot-size/pns_trap.S src/pmp-napot-size/pns_main.c
PNS_OBJS := $(PNS_SRCS:.c=.o)
PNS_OBJS := $(PNS_OBJS:.S=.o)

pmp-napot-size.elf: $(PNS_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PNS_OBJS)

# Run the PMP NAPOT size-decoding module under QEMU.
run-pmp-napot-size: pmp-napot-size.elf
	$(QEMU) -machine virt -nographic -bios none -kernel pmp-napot-size.elf

# CSR readback / ISA probe module: its own binary sharing only boot.S
# and the UART driver with the other demos. Reads misa/marchid/mimpid
# with csrr, decodes the extension bitmap, and executes one
# hand-written instruction per reported extension under a trap handler
# that records mcause/mepc on any trap.
CSR_SRCS := src/boot.S src/uart.c \
            src/csr/csr_trap.S src/csr/csr_main.c
CSR_OBJS := $(CSR_SRCS:.c=.o)
CSR_OBJS := $(CSR_OBJS:.S=.o)

csr.elf: $(CSR_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CSR_OBJS)

# Run the CSR readback / ISA probe module under QEMU.
run-csr: csr.elf
	$(QEMU) -machine virt -nographic -bios none -kernel csr.elf

# WFI wakeup-latency module: its own binary sharing boot.S, the UART
# driver, and the preempt CLINT driver with the other demos. Measures
# the latency from the CLINT timer interrupt to the first task
# instruction, with a spin baseline for comparison.
WFI_SRCS := src/boot.S src/uart.c src/preempt/clint.c \
            src/wfi-latency/wfi_trap.S src/wfi-latency/wfi_main.c
WFI_OBJS := $(WFI_SRCS:.c=.o)
WFI_OBJS := $(WFI_OBJS:.S=.o)

wfi-latency.elf: $(WFI_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(WFI_OBJS)

# Run the WFI wakeup-latency module under QEMU.
run-wfi-latency: wfi-latency.elf
	$(QEMU) -machine virt -nographic -bios none -kernel wfi-latency.elf

# WFI resume-PC module: its own binary sharing only boot.S and the UART
# driver with the other demos. Captures the exact wfi address via an
# asm numeric local label, takes a CLINT machine software interrupt
# with mepc at the wfi, and verifies mret resumes at wfi+4 with all of
# x1-x31 bit-identical, over three runs.
WFIRPC_SRCS := src/boot.S src/uart.c \
               src/wfi-resume-pc/rpc_trap.S src/wfi-resume-pc/rpc_main.c
WFIRPC_OBJS := $(WFIRPC_SRCS:.c=.o)
WFIRPC_OBJS := $(WFIRPC_SRCS:.S=.o)

wfi-resume-pc.elf: $(WFIRPC_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(WFIRPC_OBJS)

# Run the WFI resume-PC module under QEMU.
run-wfi-resume-pc: wfi-resume-pc.elf
	$(QEMU) -machine virt -nographic -bios none -kernel wfi-resume-pc.elf

# Run the misaligned-access experiment under QEMU.
run-mal: mal.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mal.elf

# Run the misaligned LR/SC experiment under QEMU.
run-amo: amo.elf
	$(QEMU) -machine virt -nographic -bios none -kernel amo.elf

# Run the amoadd.w read-modify-write module under QEMU.
run-amo-add-atomicity: amo-add-atomicity.elf
	$(QEMU) -machine virt -nographic -bios none -kernel amo-add-atomicity.elf

# Run the M-mode to U-mode trap transition module under QEMU.
run-umode: umode.elf
	$(QEMU) -machine virt -nographic -bios none -kernel umode.elf

# PLIC claim/complete module: its own binary sharing only boot.S and the
# UART driver with the other demos. Drives the PLIC directly on the virt
# machine: enables the UART interrupt (source 10), asserts it with a
# looped-back UART byte, claims it, times claim and complete with
# rdcycle, and verifies complete clears the pending state.
PLIC_SRCS := src/boot.S src/uart.c \
             src/plic/plic_main.c
PLIC_OBJS := $(PLIC_SRCS:.c=.o)
PLIC_OBJS := $(PLIC_OBJS:.S=.o)

plic.elf: $(PLIC_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PLIC_OBJS)

# Run the PLIC claim/complete module under QEMU.
run-plic: plic.elf
	$(QEMU) -machine virt -nographic -bios none -kernel plic.elf

# mtimecmp accuracy module: its own binary sharing only boot.S, the UART
# driver, and the preempt CLINT driver with the other demos. Arms
# mtimecmp 5000 ticks ahead over 1000 trials and stamps trap delivery
# with rdtime/rdcycle.
MT_SRCS := src/boot.S src/uart.c src/preempt/clint.c \
           src/mtimecmp/mt_trap.S src/mtimecmp/mt_main.c
MT_OBJS := $(MT_SRCS:.c=.o)
MT_OBJS := $(MT_OBJS:.S=.o)

mtimecmp.elf: $(MT_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MT_OBJS)

# Run the mtimecmp accuracy experiment under QEMU.
run-mtimecmp: mtimecmp.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mtimecmp.elf

# mtimecmp one-shot disarm module: its own binary sharing only boot.S,
# the UART driver, and the preempt CLINT driver with the other demos.
# Arms mtimecmp exactly one mtime tick ahead of mtime, takes the one
# machine timer interrupt, disarms by writing all-ones to mtimecmp
# inside the handler, then spins a quiet window of 1,000,000 rdcycle
# reads with interrupts enabled and requires zero re-delivery traps.
MTOS_SRCS := src/boot.S src/uart.c src/preempt/clint.c \
             src/mtimecmp-oneshot/os_trap.S src/mtimecmp-oneshot/os_main.c
MTOS_OBJS := $(MTOS_SRCS:.c=.o)
MTOS_OBJS := $(MTOS_OBJS:.S=.o)

mtimecmp-oneshot.elf: $(MTOS_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MTOS_OBJS)

# Run the mtimecmp one-shot disarm experiment under QEMU.
run-mtimecmp-oneshot: mtimecmp-oneshot.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mtimecmp-oneshot.elf

# stimecmp one-shot disarm module (S-mode): its own binary sharing only
# boot.S and the UART driver with the other demos. M-mode boot probes
# Sstc, delegates the supervisor timer interrupt via mideleg, disarms
# stimecmp once, and drops to S-mode. S-mode arms stimecmp 1000 mtime
# ticks ahead, takes the one supervisor timer interrupt (scause
# 0x8000000000000005), disarms by writing all-ones to stimecmp inside
# the S-mode handler, then spins a quiet window of 1,000,000 rdcycle
# reads with interrupts enabled and requires zero re-delivery traps.
STOS_SRCS := src/boot.S src/uart.c \
             src/stimecmp-one-shot/stos_trap.S src/stimecmp-one-shot/stos_main.c
STOS_OBJS := $(STOS_SRCS:.c=.o)
STOS_OBJS := $(STOS_OBJS:.S=.o)

stimecmp-one-shot.elf: $(STOS_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(STOS_OBJS)

# Run the stimecmp one-shot disarm experiment under QEMU.
run-stimecmp-one-shot: stimecmp-one-shot.elf
	$(QEMU) -machine virt -nographic -bios none -kernel stimecmp-one-shot.elf

# Sv39 page-table walk module: its own binary sharing only boot.S and
# the UART driver with the other demos. Builds a two-level Sv39 table
# by hand, enables it via satp, accesses a mapped page with MPRV=1 /
# MPP=S, and verifies the load-page-fault path on unmapped VAs.
SV39_SRCS := src/boot.S src/uart.c \
             src/sv39/sv39_trap.S src/sv39/sv39_main.c
SV39_OBJS := $(SV39_SRCS:.c=.o)
SV39_OBJS := $(SV39_OBJS:.S=.o)

sv39.elf: $(SV39_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SV39_OBJS)

# Run the Sv39 page-table walk module under QEMU.
run-sv39: sv39.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sv39.elf

# ecall ABI round-trip module: its own binary sharing only boot.S and
# the UART driver with the other demos. Drops to S-mode, loads known
# constants into a0-a7/t0-t6, issues an ecall per argument set, and
# verifies the M-mode handler returns the XOR of a0-a5 in a0 while
# every other register comes back bit-identical.
ECALL_SRCS := src/boot.S src/uart.c \
              src/ecall/ecall_trap.S src/ecall/ecall_main.c
ECALL_OBJS := $(ECALL_SRCS:.c=.o)
ECALL_OBJS := $(ECALL_OBJS:.S=.o)

ecall.elf: $(ECALL_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(ECALL_OBJS)

# Run the ecall ABI round-trip module under QEMU.
run-ecall: ecall.elf
	$(QEMU) -machine virt -nographic -bios none -kernel ecall.elf

# Counter-alias module: its own binary sharing only boot.S and the
# UART driver with the other demos. Reads mcycle and rdcycle back to
# back 1000 times and checks the shadow counter advances in lockstep,
# cross-checked against the CLINT mtime.
CA_SRCS := src/boot.S src/uart.c \
           src/counter-alias/ca_main.c
CA_OBJS := $(CA_SRCS:.c=.o)
CA_OBJS := $(CA_OBJS:.S=.o)

counter-alias.elf: $(CA_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CA_OBJS)

# Run the counter-alias module under QEMU.
run-counter-alias: counter-alias.elf
	$(QEMU) -machine virt -nographic -bios none -kernel counter-alias.elf

# CLINT msip software-interrupt delivery module: its own binary sharing
# only boot.S and the UART driver with the other demos. Installs an
# M-mode trap handler, sets the CLINT msip bit for hart 0, verifies
# mcause is a machine software interrupt, clears msip in the handler,
# and verifies no re-delivery, over two set/clear cycles.
MSIP_SRCS := src/boot.S src/uart.c \
             src/msip/msip_trap.S src/msip/msip_main.c
MSIP_OBJS := $(MSIP_SRCS:.c=.o)
MSIP_OBJS := $(MSIP_OBJS:.S=.o)

msip.elf: $(MSIP_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MSIP_OBJS)

# Run the CLINT msip delivery module under QEMU.
run-msip: msip.elf
	$(QEMU) -machine virt -nographic -bios none -kernel msip.elf

# mtvec vectored-dispatch module: its own binary sharing only boot.S
# and the UART driver with the other demos. Programs mtvec MODE=1
# (vectored), triggers a synchronous U-mode ecall (code 8) and a
# CLINT machine timer interrupt (code 7), and verifies each trap's
# landing address and mcause against the measured dispatch behavior
# documented in the module's PROOF.md.
MTV_SRCS := src/boot.S src/uart.c \
            src/mtvec-vectored/mtv_trap.S src/mtvec-vectored/mtv_main.c
MTV_OBJS := $(MTV_SRCS:.c=.o)
MTV_OBJS := $(MTV_OBJS:.S=.o)

mtvec-vectored.elf: $(MTV_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MTV_OBJS)

# Run the mtvec vectored-dispatch module under QEMU.
run-mtvec-vectored: mtvec-vectored.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mtvec-vectored.elf

# rdcycle monotonicity module: its own binary sharing only boot.S and
# the UART driver with the other demos. Reads rdcycle around a fixed
# 100-nop window 1000 times, checks every delta is strictly positive
# and the reads never go backward, and reports min/median/max deltas.
# On PASS it shuts the machine down via the virt test-device finisher
# so the QEMU process exit code (0) reflects the verdict; on FAIL it
# parks the hart instead.
CYCMON_SRCS := src/boot.S src/uart.c \
              src/cycmon/cyc_main.c
CYCMON_OBJS := $(CYCMON_SRCS:.c=.o)
CYCMON_OBJS := $(CYCMON_OBJS:.S=.o)

cycmon.elf: $(CYCMON_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CYCMON_OBJS)

# Run the rdcycle monotonicity module under QEMU.
run-cycmon: cycmon.elf
	$(QEMU) -machine virt -nographic -bios none -kernel cycmon.elf

# mstatus.FS write/readback module: its own binary sharing only
# boot.S and the UART driver with the other demos. Writes the FS
# field (bits 14:13) of mstatus through all four values, reads back
# the full mstatus word after each write, and reports the write/
# readback pairs plus whether any non-FS bit changed. On PASS it
# shuts the machine down via the virt test-device finisher so the
# QEMU process exit code (0) reflects the verdict; on FAIL it parks
# the hart instead.
FSCHECK_SRCS := src/boot.S src/uart.c \
                src/fs-check/fs_main.c
FSCHECK_OBJS := $(FSCHECK_SRCS:.c=.o)
FSCHECK_OBJS := $(FSCHECK_OBJS:.S=.o)

fs-check.elf: $(FSCHECK_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FSCHECK_OBJS)

# Run the mstatus.FS write/readback module under QEMU.
run-fs-check: fs-check.elf
	$(QEMU) -machine virt -nographic -bios none -kernel fs-check.elf

# mcycle write/readback module: its own binary sharing only boot.S and
# the UART driver with the other demos. Reads mcycle twice for a
# baseline, writes the constant 0x100000000, reads back immediately
# and publishes the write/readback/delta triple, then takes four
# further readbacks and requires strict monotonic advancement from
# the written base. On PASS it shuts the machine down via the virt
# test-device finisher so the QEMU process exit code (0) reflects the
# verdict; on FAIL it parks the hart instead.
MCW_SRCS := src/boot.S src/uart.c \
            src/mcycle-write/mcw_main.c
MCW_OBJS := $(MCW_SRCS:.c=.o)
MCW_OBJS := $(MCW_OBJS:.S=.o)

mcycle-write.elf: $(MCW_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MCW_OBJS)

# Run the mcycle write/readback module under QEMU.
run-mcycle-write: mcycle-write.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mcycle-write.elf

# mip/mip-msip pending-bit module: its own binary sharing only boot.S
# and the UART driver with the other demos. Writes the CLINT msip
# register for hart 0 and reads the mip CSR to verify the MSIP
# pending bit (bit 3) sets and clears with the msip write/clear,
# with the interrupt never enabled (mie.MSIE and mstatus.MIE stay
# clear), over two set/clear cycles. On PASS it shuts the machine
# down via the virt test-device finisher so the QEMU process exit
# code (0) reflects the verdict; on FAIL it parks the hart instead.
MMSP_SRCS := src/boot.S src/uart.c \
             src/mip-msip/mmsp_main.c
MMSP_OBJS := $(MMSP_SRCS:.c=.o)
MMSP_OBJS := $(MMSP_SRCS:.S=.o)

mip-msip.elf: $(MMSP_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MMSP_OBJS)

# Run the mip/mip-msip pending-bit module under QEMU.
run-mip-msip: mip-msip.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mip-msip.elf

# mip/mip-pending-no-trap module (backlog item 171): its own binary
# sharing only boot.S and the UART driver with the other demos.
# Pend the CLINT msip bit for hart 0 with the global interrupt gate
# (mstatus.MIE) and the per-interrupt enable (mie.MSIE) both clear,
# and verify the MSIP pending bit (mip bit 3) reads 1 across a
# 100,000-mcycle polling window with zero traps fired (a trap
# handler counting entries is installed to make that observable),
# then clear msip and verify mip.MSIP reads 0 again. Prints a
# checks/mismatches summary and an FNV-1a digest of the
# verdict-relevant values for run-to-run comparison. On PASS it
# shuts the machine down via the virt test-device finisher so the
# QEMU process exit code (0) reflects the verdict; on FAIL it parks
# the hart instead.
MPNT_SRCS := src/boot.S src/uart.c \
             src/mip-pending-no-trap/mpnt_trap.S src/mip-pending-no-trap/mpnt_main.c
MPNT_OBJS := $(MPNT_SRCS:.c=.o)
MPNT_OBJS := $(MPNT_SRCS:.S=.o)

mip-pending-no-trap.elf: $(MPNT_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MPNT_OBJS)

# Run the mip pending-without-trap module under QEMU.
run-mip-pending-no-trap: mip-pending-no-trap.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mip-pending-no-trap.elf

# mstatus.MIE global-gate module: its own binary sharing only boot.S
# and the UART driver with the other demos. Asserts the CLINT msip
# register for hart 0 with mstatus.MIE clear (mie.MSIE set throughout)
# and verifies the interrupt stays pending in mip with zero traps,
# then sets MIE via csrw and verifies exactly one machine software
# interrupt trap fires with the handler-recorded mcause/mepc/mtval
# and trap-entry mstatus, then clearing msip, with no re-delivery in
# a further quiet window, then re-gates with MIE clear again. On
# PASS it shuts the machine down via the virt test-device finisher
# so the QEMU process exit code (0) reflects the verdict; on FAIL it
# parks the hart instead.
MIG_SRCS := src/boot.S src/uart.c \
            src/mie-global/mig_trap.S src/mie-global/mig_main.c
MIG_OBJS := $(MIG_SRCS:.c=.o)
MIG_OBJS := $(MIG_OBJS:.S=.o)

mie-global.elf: $(MIG_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MIG_OBJS)

# Run the mstatus.MIE global-gate module under QEMU.
run-mie-global: mie-global.elf sip-ssip.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mie-global.elf sip-ssip.elf

# mie STIE/MTIE separation module: its own binary sharing boot.S,
# the UART driver, and the CLINT driver with the other demos. With
# mstatus.MIE set and a machine timer interrupt pending in mip.MTIP,
# mie = 0x20 (STIE only) must deliver zero traps because the pending
# source's own enable bit (MTIE, bit 7) is clear; the control then
# sets mie = 0xA0 (MTIE|STIE) and the still-pending MTIP must trap
# exactly once (mcause 0x8000000000000007), proving the silence was
# the enable bit. The handler disarms the timer and no re-delivery
# follows. On PASS it shuts the machine down via the virt
# test-device finisher so the QEMU process exit code (0) reflects
# the verdict; on FAIL it parks the hart instead.
STIE_SRCS := src/boot.S src/uart.c src/preempt/clint.c \
             src/mie-stie/stie_trap.S src/mie-stie/stie_main.c
STIE_OBJS := $(STIE_SRCS:.c=.o)
STIE_OBJS := $(STIE_OBJS:.S=.o)

mie-stie.elf: $(STIE_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(STIE_OBJS)

# Run the mie STIE/MTIE separation module under QEMU.
run-mie-stie: mie-stie.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mie-stie.elf

# mie.MSIE enable-gate module: its own binary sharing only boot.S and
# the UART driver, and the CLINT driver with the other demos. With
# mstatus.MIE set and the CLINT msip pended for hart 0 while MSIE is
# clear, mip.MSIP must read pending with zero traps over a bounded
# window; setting MSIE must then deliver exactly one machine
# software interrupt (mcause 0x8000000000000003). The handler clears
# the CLINT msip and no re-delivery follows. On PASS it shuts the
# machine down via the virt test-device finisher so the QEMU process
# exit code (0) reflects the verdict; on FAIL it parks the hart
# instead.
MSIE_SRCS := src/boot.S src/uart.c src/preempt/clint.c \
             src/mie-msie-gate/msie_trap.S src/mie-msie-gate/msie_main.c
MSIE_OBJS := $(MSIE_SRCS:.c=.o)
MSIE_OBJS := $(MSIE_OBJS:.S=.o)

mie-msie-gate.elf: $(MSIE_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MSIE_OBJS)

# Run the mie.MSIE enable-gate module under QEMU.
run-mie-msie-gate: mie-msie-gate.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mie-msie-gate.elf

# mie.MTIE enable-gate module: its own binary sharing only boot.S and
# the UART driver, and the CLINT driver with the other demos. With
# mstatus.MIE set and the CLINT timer armed so mip.MTIP goes pending
# while MTIE is clear, mip.MTIP must read pending with zero traps
# over a bounded window; setting MTIE must then deliver exactly one
# machine timer interrupt (mcause 0x8000000000000007). The handler
# disarms mtimecmp to all-ones and no re-delivery follows. On PASS
# it shuts the machine down via the virt test-device finisher so
# the QEMU process exit code (0) reflects the verdict; on FAIL it
# parks the hart instead.
MTIE_SRCS := src/boot.S src/uart.c src/preempt/clint.c \
             src/mie-mtie-gate/mtie_trap.S src/mie-mtie-gate/mtie_main.c
MTIE_OBJS := $(MTIE_SRCS:.c=.o)
MTIE_OBJS := $(MTIE_OBJS:.S=.o)

mie-mtie-gate.elf: $(MTIE_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MTIE_OBJS)

# Run the mie.MTIE enable-gate module under QEMU.
run-mie-mtie-gate: mie-mtie-gate.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mie-mtie-gate.elf

# mcause WARL module: its own binary sharing only boot.S and the
# UART driver with the other demos. Takes one deliberate M-mode ecall
# (mcause = 11), then writes all-ones and zero to mcause: on this
# hart both writes take effect, so the readbacks report the written
# values and all 64 bits are software-writable. A second ecall then
# confirms trap entry still overwrites the register (mcause = 11
# again). The write/last-cause/readback triples are the evidence.
# On PASS it shuts the machine down via the virt test-device finisher
# so the QEMU process exit code (0) reflects the verdict; on FAIL it
# parks the hart instead.
MCA_SRCS := src/boot.S src/uart.c \
            src/mcause-warl/mcause_trap.S src/mcause-warl/mcause_main.c
MCA_OBJS := $(MCA_SRCS:.c=.o)
MCA_OBJS := $(MCA_OBJS:.S=.o)

mcause-warl.elf: $(MCA_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MCA_OBJS)

# Run the mcause WARL module under QEMU.
run-mcause-warl: mcause-warl.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mcause-warl.elf

# scause WARL module: its own binary sharing only boot.S and the
# UART driver with the other demos. From M-mode, writes all-ones
# and zero to the S-mode cause register scause and publishes the
# legalized readbacks (both writes take on this hart, so all 64
# bits are software-writable). It then delegates supervisor
# environment calls (medeleg bit 9), writes all-ones to scause a
# final time, opens the address space with one PMP NAPOT entry,
# and drops to S-mode, where an ecall must trap to the S-mode
# handler with scause = 9, sepc exactly at the ecall site, and
# the M-mode trap count still 0, proving trap entry overwrote the
# last software write with the real cause. A deliberate illegal
# instruction hands back to M-mode for the verdict. On PASS it
# shuts the machine down via the virt test-device finisher so the
# QEMU process exit code (0) reflects the verdict; on FAIL it
# parks the hart instead.
SCW_SRCS := src/boot.S src/uart.c \
            src/scause-warl/scw_trap.S src/scause-warl/scw_strap.S src/scause-warl/scw_main.c
SCW_OBJS := $(SCW_SRCS:.c=.o)
SCW_OBJS := $(SCW_OBJS:.S=.o)

scause-warl.elf: $(SCW_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SCW_OBJS)

# Run the scause WARL module under QEMU.
run-scause-warl: scause-warl.elf
	$(QEMU) -machine virt -nographic -bios none -kernel scause-warl.elf

# sstatus.SPP record module: its own binary sharing only boot.S and
# the UART driver with the other demos. Boots in M-mode, delegates
# supervisor ecalls (medeleg bit 9) to S-mode, installs a direct-mode
# stvec, opens the address space to S-mode with one PMP NAPOT entry,
# sets sstatus.SPP=1 and mstatus.MPP=1, and srets into an S-mode
# payload. The payload issues two ecalls; each trap must enter the
# S-mode handler with sstatus.SPP=1 (the pre-trap mode) and scause=9,
# the handler records both, advances sepc by 4, and srets back. The
# payload prints the recorded values, a checksum over them, runs 10
# checks, and on PASS shuts the machine down via the virt test-device
# finisher so the QEMU process exit code (0) reflects the verdict; on
# FAIL it parks the hart instead.
SSP_SRCS := src/boot.S src/uart.c \
            src/sstatus-spp/ssp_trap.S src/sstatus-spp/ssp_main.c
SSP_OBJS := $(SSP_SRCS:.c=.o)
SSP_OBJS := $(SSP_OBJS:.S=.o)

sstatus-spp.elf: $(SSP_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SSP_OBJS)

# Run the sstatus.SPP record module under QEMU.
run-sstatus-spp: sstatus-spp.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sstatus-spp.elf

# sstatus.SPP=0 drop-to-U-mode module: its own binary sharing only
# boot.S and the UART driver with the other demos. Boots in M-mode,
# leaves medeleg at 0 (no delegation: the illegal-instruction trap
# must stay in M-mode), installs a direct-mode mtvec and a counting
# direct-mode stvec (control: must see 0 traps), opens the address
# space to U-mode with one PMP NAPOT entry R/W/X, clears sstatus.SPP,
# reads it back (must be 0), and srets to a U-mode landing pad. The
# pad records the address of its privileged read (csrr sstatus) and
# executes it; U-mode may not read sstatus, so the hart must trap to
# M-mode with mcause=2, mepc at the read site, and the trapped
# mstatus.MPP field reading 0 (U-mode). The M-mode handler records
# mcause/mepc/mtval/mstatus and jumps to a C continuation that prints
# the recorded values, a checksum over them, runs 10 checks, and on
# PASS shuts the machine down via the virt test-device finisher so
# the QEMU process exit code (0) reflects the verdict; on FAIL it
# parks the hart instead.
# NOTE: src/boot.S must stay first in SPU_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
SPU_SRCS := src/boot.S src/uart.c \
            src/sstatus-spp-sret-u/spu_trap.S src/sstatus-spp-sret-u/spu_main.c
SPU_OBJS := $(SPU_SRCS:.c=.o)
SPU_OBJS := $(SPU_OBJS:.S=.o)

sstatus-spp-sret-u.elf: $(SPU_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SPU_OBJS)

# Run the sstatus.SPP=0 drop-to-U-mode module under QEMU.
run-sstatus-spp-sret-u: sstatus-spp-sret-u.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sstatus-spp-sret-u.elf

# mideleg WARL module: its own binary sharing only boot.S and the
# UART driver with the other demos. Writes all-ones to mideleg in
# M-mode and publishes the legalized readback: the subset of
# interrupt causes this hart will let M-mode delegate to a lower
# privilege level. Then writes zero (readback must be 0) and writes
# all-ones again (readback must repeat, proving the legalization is
# stable). No interrupt source is armed and MIE stays clear, so no
# trap should ever fire; a defensive trap entry parks the hart, and
# the harness observes that as a timeout. On PASS it shuts the
# machine down via the virt test-device finisher so the QEMU process
# exit code (0) reflects the verdict; on FAIL it parks instead.
MIDW_SRCS := src/boot.S src/uart.c \
            src/mideleg-warl/mideleg_trap.S src/mideleg-warl/mideleg_main.c
MIDW_OBJS := $(MIDW_SRCS:.c=.o)
MIDW_OBJS := $(MIDW_OBJS:.S=.o)

mideleg-warl.elf: $(MIDW_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MIDW_OBJS)

# Run the mideleg WARL module under QEMU.
run-mideleg-warl: mideleg-warl.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mideleg-warl.elf

# mideleg MTIP-routing module: its own binary sharing boot.S, the
# UART driver, and the CLINT MMIO helpers with the other demos.
# Attempts to set mideleg bit 7 (machine timer interrupt delegation
# to S-mode), publishes the write/readback triple (the WARL
# legalization drops bit 7 on this hart), arms the CLINT machine
# timer, drops to S-mode, and records exactly where the interrupt
# lands. On PASS it shuts the machine down via the virt test-device
# finisher so the QEMU process exit code (0) reflects the verdict;
# on FAIL it parks instead.
MIDT_SRCS := src/boot.S src/uart.c src/preempt/clint.c \
            src/mideleg-mtip-route/mtip_trap.S src/mideleg-mtip-route/mtip_main.c
MIDT_OBJS := $(MIDT_SRCS:.c=.o)
MIDT_OBJS := $(MIDT_OBJS:.S=.o)

mideleg-mtip-route.elf: $(MIDT_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MIDT_OBJS)

# Run the mideleg MTIP-routing module under QEMU.
run-mideleg-mtip-route: mideleg-mtip-route.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mideleg-mtip-route.elf

# sepc WARL module: its own binary sharing only boot.S and the
# UART driver with the other demos. Writes all-ones and zero to
# sepc: on this hart both writes take effect, so the readbacks
# report the written values and all 64 bits are software-writable.
# A deliberate M-mode ecall then confirms trap entry overwrites the
# register with the ecall pc (the handler records it and an
# independent csrr readback agrees). The write/readback pairs are
# the evidence. On PASS it shuts the machine down via the virt
# test-device finisher so the QEMU process exit code (0) reflects
# the verdict; on FAIL it parks the hart instead.
SEPCW_SRCS := src/boot.S src/uart.c \
            src/sepc-warl/sepc_trap.S src/sepc-warl/sepc_main.c
SEPCW_OBJS := $(SEPCW_SRCS:.c=.o)
SEPCW_OBJS := $(SEPCW_OBJS:.S=.o)

sepc-warl.elf: $(SEPCW_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SEPCW_OBJS)

# Run the sepc WARL module under QEMU.
run-sepc-warl: sepc-warl.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sepc-warl.elf

# scause INTERRUPT-bit module: its own binary sharing only boot.S and
# the UART driver with the other demos. Delegates the supervisor timer
# interrupt (mideleg bit 5) and the load page fault (medeleg bit 13) to
# S-mode under an Sv39 table that identity-maps [0x80000000, 0xC0000000)
# and leaves VA 0x40000000 unmapped. In S-mode it loads from the
# unmapped address (expects scause = 13, bit 63 clear), then enables
# SIE so the armed stimecmp fires (expects scause = 0x8000000000000005,
# bit 63 set), and prints both values with a PASS/FAIL verdict.
SCB_SRCS := src/boot.S src/uart.c \
            src/scause-bit/scb_trap.S src/scause-bit/scb_main.c
SCB_OBJS := $(SCB_SRCS:.c=.o)
SCB_OBJS := $(SCB_OBJS:.S=.o)

scause-bit.elf: $(SCB_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SCB_OBJS)

# Run the scause INTERRUPT-bit module under QEMU.
run-scause-bit: scause-bit.elf
	$(QEMU) -machine virt -nographic -bios none -kernel scause-bit.elf

# sip SSIP bit module: its own binary sharing only boot.S and the
# UART driver with the other demos. Reads sip at boot as the
# baseline, sets bit 1 (SSIP) with csrs and verifies the readback
# shows bit 1 set with all other bits unchanged, clears it with csrc
# and verifies the readback returns byte-identical to the baseline,
# over two set/clear cycles. A trap handler recording
# mcause/mepc/mtval is installed but must never fire (trap count 0),
# with mie.MSIE and mstatus.MIE read back clear at boot and at the
# end. On PASS it shuts the machine down via the virt test-device
# finisher so the QEMU process exit code (0) reflects the verdict;
# on FAIL it parks the hart instead.
SIPSRCS_SRCS := src/boot.S src/uart.c \
                src/sip-ssip/sip_trap.S src/sip-ssip/sip_main.c
SIPSRCS_OBJS := $(SIPSRCS_SRCS:.c=.o)
SIPSRCS_OBJS := $(SIPSRCS_OBJS:.S=.o)

sip-ssip.elf: $(SIPSRCS_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SIPSRCS_OBJS)

# Run the sip SSIP bit module under QEMU.
run-sip-ssip: sip-ssip.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sip-ssip.elf

# mstatus.MPP encoding module: its own binary sharing only boot.S and
# the UART driver with the other demos. Writes the MPP field (bits
# 12:11) through all four encodings (00, 01, 10, 11), reads back what
# the CSR actually holds after each write, mrets into a
# one-instruction ecall payload, and verifies the trap's
# mcause/mepc/mtval and the trap-entry MPP bits against the written
# encoding. The reserved 10 encoding is measured as written: on QEMU
# 8.2.2 the write is coerced to 00 at write time, so that phase
# records the coercion and the resulting U-mode trap. On PASS it
# shuts the machine down via the virt test-device finisher so the
# QEMU process exit code (0) reflects the verdict; on FAIL it parks
# the hart instead.
MPPENC_SRCS := src/boot.S src/uart.c \
               src/mpp-encoding/mpp_trap.S src/mpp-encoding/mpp_main.c
MPPENC_OBJS := $(MPPENC_SRCS:.c=.o)
MPPENC_OBJS := $(MPPENC_OBJS:.S=.o)

mpp-encoding.elf: $(MPPENC_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MPPENC_OBJS)

# Run the mstatus.MPP encoding module under QEMU.
run-mpp-encoding: mpp-encoding.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mpp-encoding.elf

# rdcycle read-latency-floor module: its own binary sharing only boot.S
# and the UART driver with the other demos. Reads the `cycle` CSR back
# to back with zero instructions between reads (bursts of 27, one read
# per register, a single volatile asm block per burst), 38 bursts, and
# publishes the min/median/max delta between successive reads, a
# 16-bin histogram, and an rdcycle-vs-mtime calibration so the deltas
# are interpretable. A minimal M-mode trap handler records
# mcause/mepc/mtval and parks the hart on any trap. On PASS it shuts
# the machine down via the virt test-device finisher so the QEMU
# process exit code (0) reflects the verdict; on FAIL it parks the
# hart instead.
CRL_SRCS := src/boot.S src/uart.c \
            src/cycle-read-latency/crl_trap.S src/cycle-read-latency/crl_main.c
CRL_OBJS := $(CRL_SRCS:.c=.o)
CRL_OBJS := $(CRL_OBJS:.S=.o)

cycle-read-latency.elf: $(CRL_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CRL_OBJS)

# Run the rdcycle read-latency module under QEMU.
run-cycle-read-latency: cycle-read-latency.elf
	$(QEMU) -machine virt -nographic -bios none -kernel cycle-read-latency.elf

# mtvec direct-mode module: its own binary sharing only boot.S and
# the UART driver with the other demos. Writes mtvec with MODE=0
# (direct), reads it back to verify the mode bits read 0 and the
# base matches the single trap entry, then provokes two real traps:
# a deliberate M-mode ecall (mcause 9) and a machine timer interrupt
# (mcause 0x8000000000000007) armed via the CLINT mtimecmp. Both must
# land at the single BASE entry, not BASE + 4*cause; the handler
# records the address it actually entered through, cross-checked
# against the mtvec BASE readback, and any trap with another mcause
# is counted as unexpected. On PASS it shuts the machine down via
# the virt test-device finisher so the QEMU process exit code (0)
# reflects the verdict; on FAIL it parks the hart instead.
D0_SRCS := src/boot.S src/uart.c \
           src/mtvec-mode0-direct/d0_trap.S src/mtvec-mode0-direct/d0_main.c
D0_OBJS := $(D0_SRCS:.c=.o)
D0_OBJS := $(D0_OBJS:.S=.o)

mtvec-mode0-direct.elf: $(D0_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(D0_OBJS)

# Run the mtvec direct-mode module under QEMU.
run-mtvec-mode0-direct: mtvec-mode0-direct.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mtvec-mode0-direct.elf

# mscratch csrrw round-trip module: its own binary sharing only boot.S
# and the UART driver with the other demos. Records the boot mscratch
# value, csrrw-swaps a nonzero sentinel in and checks both sides of
# the atomic swap (rd holds the old value, mscratch reads back the
# sentinel), then restores the boot value exactly and verifies the
# restoration, with a trap counter proving 0 traps fired.
MSRC_SRCS := src/boot.S src/uart.c \
             src/mscratch-csrrw/msc_trap.S src/mscratch-csrrw/mscratch_main.c
MSRC_OBJS := $(MSRC_SRCS:.c=.o)
MSRC_OBJS := $(MSRC_OBJS:.S=.o)

mscratch-csrrw.elf: $(MSRC_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MSRC_OBJS)

# Run the mscratch csrrw round-trip module under QEMU.
run-mscratch-csrrw: mscratch-csrrw.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mscratch-csrrw.elf

# sstatus.SIE gate module (backlog item 176): its own binary sharing
# only boot.S and the UART driver with the other demos. Proves
# sstatus.SIE gates S-mode interrupt delivery independently of the
# pending bits: phase A arms stimecmp with SIE clear and requires sip
# STIP to go pending while zero traps fire across a 200,000-rdcycle
# window; phase B sets SIE=1 and requires exactly one trap with scause
# 0x8000000000000005 (the handler disarms stimecmp to all-ones), then a
# quiet window with SIE still on requires no re-delivery. On PASS it
# shuts the machine down via the virt test-device finisher so the
# QEMU process exit code (0) reflects the verdict; on FAIL it parks
# the hart instead.
SSG_SRCS := src/boot.S src/uart.c \
            src/sstatus-sie-gate/ssg_trap.S src/sstatus-sie-gate/ssg_main.c
SSG_OBJS := $(SSG_SRCS:.c=.o)
SSG_OBJS := $(SSG_OBJS:.S=.o)

sstatus-sie-gate.elf: $(SSG_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SSG_OBJS)

# Run the sstatus.SIE gate module under QEMU.
run-sstatus-sie-gate: sstatus-sie-gate.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sstatus-sie-gate.elf

# sie.STIE gate module: its own binary sharing only boot.S and the UART
# driver with the other demos. Proves sie.STIE gates the supervisor
# timer interrupt independently of the global sstatus.SIE gate: phase A
# opens SIE but leaves STIE clear, arms stimecmp, and requires sip STIP
# to go pending while zero traps fire across a 100,000-rdcycle window;
# phase B sets STIE=1 (via csrs, since bit 5 is not encodable in the
# csrsi immediate) and requires exactly one trap with scause
# 0x8000000000000005 (the handler disarms stimecmp to all-ones), then a
# quiet window with STIE still on requires no re-delivery. On PASS it
# shuts the machine down via the virt test-device finisher so the
# QEMU process exit code (0) reflects the verdict; on FAIL it parks
# the hart instead.
STG_SRCS := src/boot.S src/uart.c \
            src/sie-stie-gate/stg_trap.S src/sie-stie-gate/stg_main.c
STG_OBJS := $(STG_SRCS:.c=.o)
STG_OBJS := $(STG_OBJS:.S=.o)

sie-stie-gate.elf: $(STG_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(STG_OBJS)

# Run the sie.STIE gate module under QEMU.
run-sie-stie-gate: sie-stie-gate.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sie-stie-gate.elf

# sie.STIE WARL probe module (proof-backlog item "riscv sie-stie-write"):
# its own binary sharing only boot.S and the UART driver with the other
# demos. A pure M-mode CSR write/legalized-readback probe of the sie
# enable bits, the register-legalization sibling of sie-stie-gate
# (which tests interrupt-gating behavior, not register legalization):
# records the sie/mideleg boot baselines, delegates only the
# supervisor timer interrupt (mideleg bit 5), writes all-ones to sie
# and publishes the legalized readback (only the delegated STIE
# sticks; SSIE/SEIE stay clear because SSI/SEI remain M-mode
# interrupts), round-trips STIE through csrs/csrc set/clear with the
# bit reading back as written, then restores sie to 0 and mideleg to
# its boot value. No interrupt source is armed and no trap is
# expected; a counting park-on-entry trap vector makes any stray trap
# observable. On PASS it shuts the machine down via the virt
# test-device finisher so the QEMU process exit code (0) reflects the
# verdict; on FAIL it parks the hart instead.
STW_SRCS := src/boot.S src/uart.c \
            src/sie-stie-write/stw_trap.S src/sie-stie-write/stw_main.c
STW_OBJS := $(STW_SRCS:.c=.o)
STW_OBJS := $(STW_OBJS:.S=.o)

sie-stie-write.elf: $(STW_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(STW_OBJS)

# Run the sie.STIE WARL probe module under QEMU.
run-sie-stie-write: sie-stie-write.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sie-stie-write.elf

# mip.MSIP software-write probe module (proof-backlog item "riscv mip-msip-write"):
# its own binary sharing only boot.S and the UART driver with the other
# demos. Verified finding on QEMU 8.2.2: mip bit 3 (MSIP) is NOT
# software-writable through the CSR; csrsi/csrci writes are ignored
# (WARL) and the readback is unchanged, while the same immediate-form
# write sets mip.SSIP (bit 1) and reads back, proving the write path
# works and the ignored write is specific to MSIP. The bit is driven
# by the CLINT msip MMIO register instead, which is the mechanism
# mip-pending-no-trap exercises; the two modules test different
# mechanisms. The pending MTIP (boot mip 0x80) stays undelivered with
# mie.MSIE and mstatus.MIE clear across a bounded spin window: zero
# traps taken. A counting park-on-entry trap vector makes any stray
# trap observable. On PASS it shuts the machine down via the virt
# test-device finisher so the QEMU process exit code (0) reflects the
# verdict; on FAIL it parks the hart instead.
MMSW_SRCS := src/boot.S src/uart.c \
             src/mip-msip-write/mmsw_trap.S src/mip-msip-write/mmsw_main.c
MMSW_OBJS := $(MMSW_SRCS:.c=.o)
MMSW_OBJS := $(MMSW_OBJS:.S=.o)

mip-msip-write.elf: $(MMSW_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MMSW_OBJS)

# Run the mip.MSIP software-write probe module under QEMU.
run-mip-msip-write: mip-msip-write.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mip-msip-write.elf

# mstatus.SIE global-gate module: its own binary sharing only boot.S
# and the UART driver with the other demos. Proves the S-mode
# interrupt gate bit is drivable from M-mode through the mstatus CSR:
# the same physical bit sstatus-sie-gate drives from S-mode (sstatus
# is a subset view of mstatus, so the two names address one bit;
# this module verifies the aliasing by reading every mstatus.SIE
# write back through sstatus too), distinct in kind from the
# per-interrupt sie.STIE enable tested by sie-stie-gate: phase 1
# clears mstatus.SIE, pends the delegated supervisor software
# interrupt, and requires zero S-mode traps across a bounded rdcycle
# window in S-mode while the pending bit stays set; phase 2 sets
# mstatus.SIE and requires exactly one S-mode trap with scause
# 0x8000000000000001 and sepc inside the landing pad range, after
# which the handler clears the pending bit and the rest of the window
# is quiet. On PASS it shuts the machine down via the virt
# test-device finisher so the QEMU process exit code (0) reflects the
# verdict; on FAIL it parks the hart instead.
MSG_SRCS := src/boot.S src/uart.c \
            src/mstatus-sie-toggle/msg_trap.S src/mstatus-sie-toggle/msg_main.c
MSG_OBJS := $(MSG_SRCS:.c=.o)
MSG_OBJS := $(MSG_OBJS:.S=.o)

mstatus-sie-toggle.elf: $(MSG_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MSG_OBJS)

# Run the mstatus.SIE gate module under QEMU.
run-mstatus-sie-toggle: mstatus-sie-toggle.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mstatus-sie-toggle.elf

# sstatus.SUM gate module (proof-backlog item "sstatus-sum-probe"): its
# own binary sharing only boot.S and the UART driver with the other
# demos. Builds a minimal Sv39 table by hand (identity megapages for
# code/data/UART, one 4 KiB U=1,R=1 leaf mapping VA 0x40000000 to a
# canary page), delegates the load page fault (medeleg bit 13) to
# S-mode, and mret drops into S-mode. Phase A with sstatus.SUM clear
# does one ld from the U VA and requires exactly one trap with scause
# 0xd, stval = the faulting VA, sepc = the faulting load (the handler
# advances sepc by 4 and srets; a poisoned t0 proves the ld never
# completed). Phase B sets SUM via csrs and requires the same ld to
# return the canary with no new trap. On PASS it shuts the machine
# down via the virt test-device finisher so the QEMU process exit
# code (0) reflects the verdict; on FAIL it parks the hart instead.
SSUM_SRCS := src/boot.S src/uart.c \
            src/sstatus-sum/ssum_trap.S src/sstatus-sum/ssum_main.c
SSUM_OBJS := $(SSUM_SRCS:.c=.o)
SSUM_OBJS := $(SSUM_OBJS:.S=.o)

sstatus-sum.elf: $(SSUM_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SSUM_OBJS)

# Run the sstatus.SUM gate module under QEMU.
run-sstatus-sum: sstatus-sum.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sstatus-sum.elf

# sip.STIP software-write probe (backlog item riscv sip-stip-write): its
# own binary sharing only boot.S and the UART driver with the other
# demos. In S-mode with Sstc, disarms stimecmp to all-ones first, then
# pends the supervisor timer interrupt by writing sip STIP and requires
# the readback to stick; the one trap must arrive with scause
# 0x8000000000000005 and sepc inside the labeled wait region, the
# handler clears STIP and a quiet window requires no re-delivery. If
# the STIP write does not stick, the module ships the measured
# WARL-ignore result instead of forcing the premise. On PASS it shuts
# the machine down via the virt test-device finisher so the QEMU
# process exit code (0) reflects the verdict; on FAIL it parks the
# hart instead.
SSW_SRCS := src/boot.S src/uart.c \
            src/sip-stip-write/ssw_trap.S src/sip-stip-write/ssw_main.c
SSW_OBJS := $(SSW_SRCS:.c=.o)
SSW_OBJS := $(SSW_OBJS:.S=.o)

sip-stip-write.elf: $(SSW_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SSW_OBJS)

# Run the sip.STIP software-write probe under QEMU.
run-sip-stip-write: sip-stip-write.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sip-stip-write.elf

# sip.SEIP read-only probe (backlog item riscv sip-seip-write): its
# own binary sharing only boot.S and the UART driver with the other
# demos. M-mode installs a counting trap vector, delegates nothing
# (mideleg stays zero), opens memory with one PMP NAPOT entry, and
# mret drops to S-mode. S-mode records the sip readback, writes
# all-ones to sip, and requires SEIP (bit 9) to read back unchanged
# while the software-writable SSIP bit sticks (proving the write
# executed); the M-mode trap count must stay 0. A zero write must
# clear SSIP/STIP with SEIP still unchanged. On PASS it shuts the
# machine down via the virt test-device finisher so the QEMU process
# exit code (0) reflects the verdict; on FAIL it parks the hart.
SEIPW_SRCS := src/boot.S src/uart.c \
              src/sip-seip-write/seipw_trap.S src/sip-seip-write/seipw_main.c
SEIPW_OBJS := $(SEIPW_SRCS:.c=.o)
SEIPW_OBJS := $(SEIPW_OBJS:.S=.o)

sip-seip-write.elf: $(SEIPW_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SEIPW_OBJS)

# Run the sip.SEIP read-only probe under QEMU.
run-sip-seip-write: sip-seip-write.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sip-seip-write.elf

# sip.STIP vs mideleg bit 5 reconciliation probe (backlog item riscv
# sip-stip-mideleg-reconcile): its own binary sharing only boot.S and
# the UART driver with the other demos. M-mode installs a counting
# trap vector, delegates only SSI (mideleg bit 1), and mret drops to
# S-mode. Phase A writes all-ones and zero to sip with mideleg bit 5
# (STI) CLEAR; a deliberate ecall then raises bit 5 in the M-mode
# handler (trap count must be exactly 1, mcause 9), and Phase B
# repeats the writes with STI delegated. SSIP sticking in each phase
# proves the write executed; the module publishes both readbacks,
# both mideleg values, and requires the STIP-stuck flag to be
# identical in both configurations. On PASS it shuts the machine down
# via the virt test-device finisher so the QEMU process exit code (0)
# reflects the verdict; on FAIL it parks the hart.
STRM_SRCS := src/boot.S src/uart.c \
             src/sip-stip-mideleg-reconcile/strm_trap.S src/sip-stip-mideleg-reconcile/strm_main.c
STRM_OBJS := $(STRM_SRCS:.c=.o)
STRM_OBJS := $(STRM_OBJS:.S=.o)

sip-stip-mideleg-reconcile.elf: $(STRM_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(STRM_OBJS)

# Run the sip.STIP vs mideleg bit 5 reconciliation probe under QEMU.
run-sip-stip-mideleg-reconcile: sip-stip-mideleg-reconcile.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sip-stip-mideleg-reconcile.elf

# medeleg bit-2 illegal-instruction trap destination switch module
# (backlog item "riscv medeleg-illegal-inst-route"): its own binary
# sharing only boot.S and the UART driver with the other demos.
# Phase 1 runs with medeleg written to 0 (bit 2 verified clear on
# readback) and the same 0xffffffff illegal word issued from S-mode
# must trap in M-mode with mcause = 2 and mepc at the word; the
# M-mode handler redirects to the phase-2 setup. Phase 2 runs with
# medeleg bit 2 set and the same word must trap in S-mode with
# scause = 2 and sepc at the word, resuming after it. Per-mode trap
# counts, mtval/stval, the medeleg write/readback values, and an
# FNV-1a checksum over the verdict values are printed; all interrupt
# enables stay clear, so no interrupt can fire in either phase.
# PASS writes 0x5555 to the virt test-device finisher so the QEMU
# exit code (0) reflects the verdict; on FAIL it parks the hart.
# NOTE: src/boot.S must stay first in MILI_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
MILI_SRCS := src/boot.S src/uart.c \
             src/medeleg-illegal-inst-route/mili_trap.S src/medeleg-illegal-inst-route/mili_main.c
MILI_OBJS := $(MILI_SRCS:.c=.o)
MILI_OBJS := $(MILI_OBJS:.S=.o)

medeleg-illegal-inst-route.elf: $(MILI_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MILI_OBJS)

# Run the medeleg illegal-instruction routing module under QEMU.
run-medeleg-illegal-inst-route: medeleg-illegal-inst-route.elf
	$(QEMU) -machine virt -nographic -bios none -kernel medeleg-illegal-inst-route.elf

# medeleg bit-3 breakpoint trap destination switch module
# (backlog item "riscv medeleg-breakpoint-route"): its own binary
# sharing only boot.S and the UART driver with the other demos.
# Phase 1 runs with medeleg written to 0 (bit 3 verified clear on
# readback) and the same ebreak issued from S-mode must trap in
# M-mode with mcause = 3 and mepc at the ebreak; the M-mode handler
# redirects to the phase-2 setup. Phase 2 runs with medeleg bit 3
# set (readback must be exactly 0x8) and the same ebreak must trap
# in S-mode with scause = 3 and sepc at the ebreak, resuming after
# it. Per-mode trap counts, mtval/stval, the medeleg write/readback
# values, and an FNV-1a checksum over the verdict values are
# printed; all interrupt enables stay clear, so no interrupt can
# fire in either phase. PASS writes 0x5555 to the virt test-device
# finisher so the QEMU exit code (0) reflects the verdict; on FAIL
# it parks the hart.
# NOTE: src/boot.S must stay first in MEBK_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
MEBK_SRCS := src/boot.S src/uart.c \
             src/medeleg-breakpoint/mebk_trap.S src/medeleg-breakpoint/mebk_main.c
MEBK_OBJS := $(MEBK_SRCS:.c=.o)
MEBK_OBJS := $(MEBK_OBJS:.S=.o)

medeleg-breakpoint.elf: $(MEBK_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MEBK_OBJS)

# Run the medeleg breakpoint routing module under QEMU.
run-medeleg-breakpoint: medeleg-breakpoint.elf
	$(QEMU) -machine virt -nographic -bios none -kernel medeleg-breakpoint.elf

# mcause interrupt-bit module: its own binary sharing only boot.S and
# the UART driver with the other demos. Records mcause for a
# deliberate M-mode ecall (expect 0xb, bit 63 clear: a synchronous
# exception) and for a CLINT machine timer interrupt (expect
# 0x8000000000000007, bit 63 set: an asynchronous interrupt),
# publishing (mcause >> 63) for each. The timer is armed with the
# bounded retry used by the mtvec-vectored module, disarmed to
# all-ones by the handler, and a quiet window proves no re-delivery.
# Every expectation is an in-program check; on PASS the module prints
# done and parks in wfi (the bench harness runs QEMU under timeout).
MCB_SRCS := src/boot.S src/uart.c \
            src/mcause-interrupt-bit/mib_trap.S src/mcause-interrupt-bit/mib_main.c
MCB_OBJS := $(MCB_SRCS:.c=.o)
MCB_OBJS := $(MCB_OBJS:.S=.o)

mcause-interrupt-bit.elf: $(MCB_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MCB_OBJS)

# Run the mcause interrupt-bit module under QEMU.
run-mcause-interrupt-bit: mcause-interrupt-bit.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mcause-interrupt-bit.elf

clean:
	rm -f $(OBJS) $(PREEMPT_OBJS) $(VIRTIO_OBJS) $(SMP_OBJS) $(SHELL_OBJS) $(UARTBAUD_OBJS) $(SMODE_S_OBJS) $(SMODE_M_OBJS) $(PMP_OBJS) $(WFI_OBJS) $(MIS_OBJS) $(PLIC_OBJS) $(MT_OBJS) $(SV39_OBJS) $(ECALL_OBJS) $(CA_OBJS) $(AMO_OBJS) $(AAA_OBJS) $(UMODE_OBJS) $(MSIP_OBJS) $(MTV_OBJS) $(CYCMON_OBJS) $(FSCHECK_OBJS) $(MDEL_OBJS) $(WFIRPC_OBJS) $(PMPTOR_OBJS) $(MPPENC_OBJS) $(MCW_OBJS) $(MMSP_OBJS) $(MIG_OBJS) $(MNR_OBJS) $(SVD_OBJS) $(MRS_OBJS) $(SS_OBJS) $(PNS_OBJS) $(SATPA_OBJS) $(SATPB_OBJS) $(MFA_OBJS) $(MCE_OBJS) $(CRL_OBJS) $(MTOS_OBJS) $(STOS_OBJS) $(STIE_OBJS) $(MCA_OBJS) $(MIDR_OBJS) $(SVV_OBJS) $(SCB_OBJS) $(MIDW_OBJS) $(MIDT_OBJS) $(D0_OBJS) $(PLB_OBJS) $(MSRC_OBJS) $(METOG_OBJS) $(SSP_OBJS) $(SPU_OBJS) $(SIPW_OBJS) $(MPNT_OBJS) $(SSG_OBJS) $(STG_OBJS) $(SSW_OBJS) $(STW_OBJS) $(MMSW_OBJS) $(MSG_OBJS) $(MCB_OBJS) $(SCW_OBJS) $(SSUM_OBJS) $(MXR_OBJS) $(SCCY_OBJS) $(SEIPW_OBJS) $(SCIR_OBJS) $(MSSR_OBJS) $(MDT_OBJS) $(MEDE_OBJS) $(UMUR_OBJS) $(SEIP_OBJS) $(STRM_OBJS) sip-stip-mideleg-reconcile.elf demo.elf preempt.elf virtio-blk.elf smp.elf shell.elf uart-baud.elf smode.elf smode-mbase.elf pmp.elf wfi-latency.elf mal.elf plic.elf mtimecmp.elf sv39.elf ecall.elf counter-alias.elf amo.elf umode.elf msip.elf mtvec-vectored.elf cycmon.elf fs-check.elf medeleg-mask.elf wfi-resume-pc.elf pmp-tor.elf mpp-encoding.elf mcycle-write.elf mip-msip.elf mie-global.elf sip-ssip.elf mret-no-restore.elf stvec-direct.elf mepc-resume-skip.elf sepc-resume-skip.elf pmp-napot-size.elf mtval-fault-address.elf mcounteren.elf cycle-read-latency.elf mtimecmp-oneshot.elf stimecmp-one-shot.elf mie-stie.elf mcause-warl.elf mideleg-route.elf stvec-vectored.elf sepc-warl.elf scause-bit.elf mideleg-warl.elf mideleg-mtip-route.elf pmp-lock-bit.elf mie-toggle.elf mscratch-csrrw.elf sstatus-spp.elf sstatus-spp-sret-u.elf sip-write-probe.elf mip-pending-no-trap.elf sstatus-sie-gate.elf sip-stip-write.elf mcause-interrupt-bit.elf scause-warl.elf sstatus-sum.elf sie-stie-gate.elf sie-stie-write.elf mip-msip-write.elf mstatus-sie-toggle.elf sstatus-mxr.elf scounteren-cy-gate.elf amo-add-atomicity.elf sip-seip-write.elf mideleg-ssip-route.elf medeleg-ecall-destination.elf scounteren-ir-gate.elf mideleg-seip-route.elf $(MILI_OBJS) medeleg-illegal-inst-route.elf $(MEBK_OBJS) medeleg-breakpoint.elf $(STVC_OBJS) $(STEC_OBJS) $(PNEN_OBJS) pmp-napot-encode.elf stval-illegal-capture.elf stval-ecall-capture.elf $(FNX_OBJS) fflags-nx-inexact.elf $(FFU_OBJS) fflags-uf-underflow.elf $(FOF_OBJS) fflags-of-overflow.elf $(SFD_OBJS) sstatus-fs-dirty.elf $(SMW_OBJS) satp-mode-warl.elf $(SSCS_OBJS) sie-ssip-clear-suppresses.elf $(STVM_OBJS) stvec-vectored-mode.elf $(FRW_OBJS) frm-rounding-write.elf $(FDZ_OBJS) fflags-dz-divide-by-zero.elf $(FNV_OBJS) fflags-nv-invalid.elf $(FFI_OBJS) $(FFR_OBJS) fcsr-field-independence.elf fcsr-frm-roundup.elf $(FDN_OBJS) fcsr-frm-rounddn.elf
.PHONY: all run clean

# scounteren.TM U-mode rdtime gate module (backlog scounteren-tm-gate):
# its own binary sharing only boot.S and the UART driver with the
# other demos. M-mode clears scounteren, sets mcounteren.TM (so the
# M-level gate does not mask the S-level gate under test), delegates
# the illegal-instruction trap and the U-mode ecall to S-mode, and
# drops M -> S -> U twice: phase A expects the U-mode rdtime with
# TM clear to trap with scause=2, sepc exactly at the rdtime site,
# and the destination register still holding its sentinel; phase B
# sets scounteren.TM in S-mode and expects both U-mode rdtime reads
# to succeed with strictly increasing samples. Every expectation is
# an in-program check; on PASS it shuts the machine down via the
# virt test-device finisher so the QEMU process exit code (0)
# reflects the verdict; on FAIL it parks the hart instead.
SCTG_SRCS := src/boot.S src/uart.c \
            src/scounteren-tm-gate/sctg_trap.S src/scounteren-tm-gate/sctg_main.c
SCTG_OBJS := $(SCTG_SRCS:.c=.o)
SCTG_OBJS := $(SCTG_OBJS:.S=.o)

scounteren-tm-gate.elf: $(SCTG_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SCTG_OBJS)

# Run the scounteren.TM gate module under QEMU.
run-scounteren-tm-gate: scounteren-tm-gate.elf
	$(QEMU) -machine virt -nographic -bios none -kernel scounteren-tm-gate.elf

# scounteren.CY U-mode rdcycle gate module (backlog scounteren-cy-gate):
# its own binary sharing only boot.S and the UART driver with the
# other demos. M-mode clears scounteren, sets mcounteren.CY (so the
# M-level gate does not mask the S-level gate under test), delegates
# the illegal-instruction trap and the U-mode ecall to S-mode, and
# drops M -> S -> U twice: phase A expects the U-mode rdcycle with
# CY clear to trap with scause=2, sepc exactly at the rdcycle site,
# and the destination register still holding its sentinel; phase B
# sets scounteren.CY in S-mode and expects both U-mode rdcycle reads
# to succeed with strictly increasing samples. Every expectation is
# an in-program check; on PASS it shuts the machine down via the
# virt test-device finisher so the QEMU process exit code (0)
# reflects the verdict; on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in SCCY_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
SCCY_SRCS := src/boot.S src/uart.c \
            src/scounteren-cy-gate/sccy_trap.S src/scounteren-cy-gate/sccy_main.c
SCCY_OBJS := $(SCCY_SRCS:.c=.o)
SCCY_OBJS := $(SCCY_OBJS:.S=.o)

scounteren-cy-gate.elf: $(SCCY_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SCCY_OBJS)

# Run the scounteren.CY gate module under QEMU.
run-scounteren-cy-gate: scounteren-cy-gate.elf
	$(QEMU) -machine virt -nographic -bios none -kernel scounteren-cy-gate.elf

# menvcfg-stce module: its own binary sharing only boot.S and the UART
# driver with the other demos. Reads menvcfg, probes the STCE bit's
# WARL behavior (clear it, write all-ones, restore the boot value),
# then drops to S-mode and checks whether real stimecmp access agrees
# with the advertisement; when honest, it arms a supervisor timer
# interrupt from S-mode and requires exactly one delegated trap plus
# a quiet window with no re-delivery. Every expectation is an
# in-program check; on PASS the virt test-device finisher shuts the
# machine down (QEMU exit 0), on FAIL the hart parks (timeout 124).
# NOTE: src/boot.S must stay first in MENV_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
MENV_SRCS := src/boot.S src/uart.c \
             src/menvcfg-stce/menv_main.c src/menvcfg-stce/menv_trap.S
MENV_OBJS := $(MENV_SRCS:.c=.o)
MENV_OBJS := $(MENV_OBJS:.S=.o)

menvcfg-stce.elf: $(MENV_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MENV_OBJS)

# Run the menvcfg STCE advertisement probe under QEMU.
run-menvcfg-stce: menvcfg-stce.elf
	$(QEMU) -machine virt -nographic -bios none -kernel menvcfg-stce.elf

# sstatus.MXR gate module (proof-backlog item "sstatus-mxr-probe"): its
# own binary sharing only boot.S and the UART driver with the other
# demos. Builds a minimal Sv39 table by hand (identity megapages for
# code/data/UART, one 4 KiB X-only leaf mapping VA 0x40000000 to a
# canary page), delegates the load page fault (medeleg bit 13) to
# S-mode, and mret drops into S-mode. Phase A with sstatus.MXR clear
# does one ld from the X-only VA and requires exactly one trap with
# scause 0xd, stval = the faulting VA, sepc = the faulting load (the
# handler advances sepc by 4 and srets; a poisoned t0 proves the ld
# never completed). Phase B sets MXR via csrs and requires the same
# ld to return the canary with no new trap. On PASS it shuts the
# machine down via the virt test-device finisher so the QEMU process
# exit code (0) reflects the verdict; on FAIL it parks the hart
# instead.
# NOTE: src/boot.S must stay first in MXR_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
MXR_SRCS := src/boot.S src/uart.c \
            src/sstatus-mxr/mxr_trap.S src/sstatus-mxr/mxr_main.c
MXR_OBJS := $(MXR_SRCS:.c=.o)
MXR_OBJS := $(MXR_OBJS:.S=.o)

sstatus-mxr.elf: $(MXR_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MXR_OBJS)

# Run the sstatus.MXR gate module under QEMU.
run-sstatus-mxr: sstatus-mxr.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sstatus-mxr.elf

# mideleg bit-1 supervisor-software-interrupt routing module (backlog
# item "riscv mideleg-ssip-route"): its own binary sharing only boot.S
# and the UART driver with the other demos. Programs mideleg bit 1
# with readback checks (bit 9 verified clear, so only the SSI is
# delegated), drops to S-mode, shows a pending CLINT msip does NOT
# route through mideleg bit 1 (the CLINT drives mip.MSIP, cause 3,
# which bit 1 does not cover), then pends the interrupt's own source
# mip.SSIP and requires exactly one S-mode trap with
# scause = 0x8000000000000001, sepc at the interrupted instruction,
# sip showing SSIP at handler entry and clear afterwards, 0 M-mode
# traps, and a quiet window with no further traps.
# NOTE: src/boot.S must stay first in MSSR_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
MSSR_SRCS := src/boot.S src/uart.c \
             src/mideleg-ssip-route/mssr_trap.S src/mideleg-ssip-route/mssr_main.c
MSSR_OBJS := $(MSSR_SRCS:.c=.o)
MSSR_OBJS := $(MSSR_OBJS:.S=.o)

mideleg-ssip-route.elf: $(MSSR_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MSSR_OBJS)

# Run the mideleg SSIP routing module under QEMU.
run-mideleg-ssip-route: mideleg-ssip-route.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mideleg-ssip-route.elf

# mideleg bit-9 supervisor-external-interrupt routing module (backlog
# item "riscv mideleg-seip-route"): its own binary sharing only boot.S
# and the UART driver with the other demos (plus the CLINT helper for
# the machine-timer disarm). Publishes the mideleg write/readback
# triple for bit 9 (0x200, SEI): the write takes on this hart, unlike
# the bit-7 case in mideleg-mtip-route. Pends the supervisor external
# interrupt from M-mode with "csrs mip, 1<<9" (S-mode sip writes to
# bit 9 are read-only, per src/sip-seip-write), drops to S-mode, and
# requires exactly one S-mode trap with
# scause = 0x8000000000000009, sepc at the interrupted instruction,
# sip showing SEIP at handler entry, 0 M-mode traps, and a quiet
# window with no further traps. The S-mode handler clears
# sstatus.SPIE as well as SIE because it cannot clear SEIP from
# S-mode and sret restores SIE from SPIE. On PASS it shuts the
# machine down via the virt test-device finisher so the QEMU process
# exit code (0) reflects the verdict; on FAIL it parks instead.
# NOTE: src/boot.S must stay first in SEIP_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
SEIP_SRCS := src/boot.S src/uart.c src/preempt/clint.c \
             src/mideleg-seip-route/seip_trap.S src/mideleg-seip-route/seip_main.c
SEIP_OBJS := $(SEIP_SRCS:.c=.o)
SEIP_OBJS := $(SEIP_OBJS:.S=.o)

mideleg-seip-route.elf: $(SEIP_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SEIP_OBJS)

# Run the mideleg SEIP routing module under QEMU.
run-mideleg-seip-route: mideleg-seip-route.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mideleg-seip-route.elf

# medeleg bit-9 S-mode ecall destination switch module (backlog item
# "riscv medeleg-ecall-destination"): its own binary sharing only
# boot.S and the UART driver with the other demos. Phase 1 runs with
# medeleg zeroed and an S-mode ecall must trap in M-mode with
# mcause = 9 (environment call from S-mode) and mepc at the ecall;
# the M-mode handler redirects to the phase-2 setup. Phase 2 runs
# with medeleg bit 9 set and the same ecall must trap in S-mode
# with scause = 9 and sepc at the ecall, resuming after it. Per-mode
# trap counts for both phases and an FNV-1a checksum over the
# verdict values are printed; all interrupt enables stay clear, so
# no interrupt can fire in either phase.
# NOTE: src/boot.S must stay first in MEDE_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
MEDE_SRCS := src/boot.S src/uart.c \
             src/medeleg-ecall-destination/mede_trap.S src/medeleg-ecall-destination/mede_main.c
MEDE_OBJS := $(MEDE_SRCS:.c=.o)
MEDE_OBJS := $(MEDE_OBJS:.S=.o)

medeleg-ecall-destination.elf: $(MEDE_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MEDE_OBJS)

# Run the medeleg ecall-destination module under QEMU.
run-medeleg-ecall-destination: medeleg-ecall-destination.elf mie-msie-gate.elf mie-mtie-gate.elf
	$(QEMU) -machine virt -nographic -bios none -kernel medeleg-ecall-destination.elf mie-msie-gate.elf mie-mtie-gate.elf

# medeleg bit-8 U-mode ecall route module (backlog item
# "riscv medeleg-ecall-u-route"): its own binary sharing only
# boot.S and the UART driver with the other demos. M-mode writes
# 0x100 to medeleg, drops to U-mode via sret with SPP = 0, and the
# single U-mode ecall must trap in S-mode with scause = 8
# (environment call from U-mode) and sepc at the ecall; no M-mode
# trap may fire during the delegation phase. The S-mode reporter
# then issues a restore ecall from S-mode (bit 9 clear, so M-mode
# takes it), whose handler writes the boot medeleg value back.
# An FNV-1a checksum over the verdict values is printed; all
# interrupt enables stay clear, so no interrupt can fire.
# NOTE: src/boot.S must stay first in UMUR_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
UMUR_SRCS := src/boot.S src/uart.c \
             src/medeleg-ecall-u-route/umede_trap.S src/medeleg-ecall-u-route/umede_main.c
UMUR_OBJS := $(UMUR_SRCS:.c=.o)
UMUR_OBJS := $(UMUR_OBJS:.S=.o)

medeleg-ecall-u-route.elf: $(UMUR_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(UMUR_OBJS)

# Run the medeleg U-mode ecall route module under QEMU.
run-medeleg-ecall-u-route: medeleg-ecall-u-route.elf
	$(QEMU) -machine virt -nographic -bios none -kernel medeleg-ecall-u-route.elf

# scounteren.IR U-mode rdinstret gate module (backlog riscv scounteren-ir-gate):
# its own binary sharing only boot.S and the UART driver with the
# other demos. M-mode clears scounteren, sets mcounteren.IR (so the
# M-level gate does not mask the S-level gate under test), delegates
# the illegal-instruction trap and the U-mode ecall to S-mode, and
# drops M -> S -> U twice: phase A expects the U-mode rdinstret with
# IR clear to trap with scause=2, sepc exactly at the rdinstret site,
# and the destination register still holding its sentinel; phase B
# sets scounteren.IR in S-mode and expects both U-mode rdinstret
# reads to succeed with strictly increasing samples (a 16-nop
# sequence separates the reads; on QEMU 8.2.2 without -icount the
# instret counter advances with host time, documented in PROOF.md).
# Every expectation is an in-program check; on PASS it shuts the
# machine down via the virt test-device finisher so the QEMU
# process exit code (0) reflects the verdict; on FAIL it parks
# the hart instead.
# NOTE: src/boot.S must stay first in SCIR_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
SCIR_SRCS := src/boot.S src/uart.c \
            src/scounteren-ir-gate/scir_trap.S src/scounteren-ir-gate/scir_main.c
SCIR_OBJS := $(SCIR_SRCS:.c=.o)
SCIR_OBJS := $(SCIR_OBJS:.S=.o)

scounteren-ir-gate.elf: $(SCIR_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SCIR_OBJS)

# Run the scounteren.IR gate module under QEMU.
run-scounteren-ir-gate: scounteren-ir-gate.elf
	$(QEMU) -machine virt -nographic -bios none -kernel scounteren-ir-gate.elf

# mcountinhibit.CY gate module: its own binary sharing only boot.S and
# the UART driver with the other demos. Runs entirely in M-mode on
# hart 0 (QEMU boots the ELF straight into M-mode with -bios none):
# records mcycle, writes mcountinhibit with CY (bit 0) set and
# requires the readback to carry the bit, then samples mcycle 1000
# times and requires zero advance (every readback identical); clears
# CY, requires the zero readback, then samples mcycle again in
# bounded spins and requires strictly increasing samples. A minimal
# M-mode trap handler records mcause/mepc/mtval and a trap count,
# then parks the hart; any trap is unexpected, so a printed PASS
# implies zero traps. On PASS the machine shuts down via the virt
# test-device finisher so the QEMU process exit code (0) reflects
# the verdict; on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in MCYI_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
MCYI_SRCS := src/boot.S src/uart.c \
            src/mcountinhibit-cy-gate/mcy_trap.S src/mcountinhibit-cy-gate/mcy_main.c
MCYI_OBJS := $(MCYI_SRCS:.c=.o)
MCYI_OBJS := $(MCYI_OBJS:.S=.o)

mcountinhibit-cy-gate.elf: $(MCYI_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MCYI_OBJS)

# Run the mcountinhibit.CY gate module under QEMU.
run-mcountinhibit-cy-gate: mcountinhibit-cy-gate.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mcountinhibit-cy-gate.elf

# mcountinhibit.IR gate module (backlog riscv mcountinhibit-ir-freeze):
# its own binary sharing only boot.S and the UART driver with the
# other demos. Runs entirely in M-mode on hart 0 (QEMU boots the ELF
# straight into M-mode with -bios none): records minstret, writes
# mcountinhibit with IR (bit 2) set and requires the readback to
# carry the bit, then samples minstret 1000 times and requires zero
# advance (every readback identical); clears IR, requires the zero
# readback, then samples minstret again in bounded spins and
# requires strictly increasing samples. A minimal M-mode trap
# handler records mcause/mepc/mtval and a trap count, then parks
# the hart; any trap is unexpected, so a printed PASS implies zero
# traps. On PASS the machine shuts down via the virt test-device
# finisher so the QEMU process exit code (0) reflects the verdict;
# on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in MIRF_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
MIRF_SRCS := src/boot.S src/uart.c \
            src/mcountinhibit-ir-freeze/mirf_trap.S src/mcountinhibit-ir-freeze/mirf_main.c
MIRF_OBJS := $(MIRF_SRCS:.c=.o)
MIRF_OBJS := $(MIRF_OBJS:.S=.o)

mcountinhibit-ir-freeze.elf: $(MIRF_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MIRF_OBJS)

# Run the mcountinhibit.IR gate module under QEMU.
run-mcountinhibit-ir-freeze: mcountinhibit-ir-freeze.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mcountinhibit-ir-freeze.elf

# mtime advance/delta module: its own binary sharing only boot.S and
# the UART driver with the other demos. Runs on hart 0 in M-mode
# (QEMU boots the ELF straight into M-mode with -bios none): reads
# the CLINT mtime (0x0200bff8) with 32-bit stable-pair loads only
# (64-bit CLINT accesses fault on this emulator), takes 12 read
# pairs separated by a spin of at least 10000 ticks requiring a
# strictly positive delta, takes 8 immediate back-to-back pairs
# requiring no backward step, and requires a zero trap count. A
# minimal M-mode trap handler records mcause/mepc/mtval and a trap
# count, then parks the hart; any trap is unexpected, so a printed
# PASS implies zero traps. On PASS the machine shuts down via the
# virt test-device finisher so the QEMU process exit code (0)
# reflects the verdict; on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in MDT_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
MDT_SRCS := src/boot.S src/uart.c \
            src/mtimecmp-delta-tracks-mtime/mdt_trap.S src/mtimecmp-delta-tracks-mtime/mdt_main.c
MDT_OBJS := $(MDT_SRCS:.c=.o)
MDT_OBJS := $(MDT_OBJS:.S=.o)

mtimecmp-delta-tracks-mtime.elf: $(MDT_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MDT_OBJS)

# Run the mtime advance/delta module under QEMU.
run-mtimecmp-delta-tracks-mtime: mtimecmp-delta-tracks-mtime.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mtimecmp-delta-tracks-mtime.elf

# mtimecmp-rw module (backlog riscv mtimecmp-rw):
# its own binary sharing only boot.S and the UART driver with the
# other demos. Runs on hart 0 (QEMU boots the ELF straight into
# M-mode with -bios none): writes three patterns (0x0,
# 0x123456789ABCDEF0, all-ones) to the CLINT mtimecmp register at
# 0x02004000 with two 32-bit stores each and requires each
# readback to match exactly, sampling mip.MTIP after each write.
# All mie bits stay clear the whole run so no machine timer
# interrupt can be delivered even when MTIP pends; a minimal trap
# handler records and parks on any trap, so a printed PASS implies
# zero traps. On PASS the machine shuts down via the virt
# test-device finisher so the QEMU process exit code (0) reflects
# the verdict; on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in MRW_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
MRW_SRCS := src/boot.S src/uart.c \
            src/mtimecmp-rw/rw_trap.S src/mtimecmp-rw/rw_main.c
MRW_OBJS := $(MRW_SRCS:.c=.o)
MRW_OBJS := $(MRW_OBJS:.S=.o)

mtimecmp-rw.elf: $(MRW_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MRW_OBJS)

# Run the mtimecmp read/write probe under QEMU.
run-mtimecmp-rw: mtimecmp-rw.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mtimecmp-rw.elf

# stval-illegal-capture module (backlog riscv stval-illegal-capture):
# its own binary sharing only boot.S and the UART driver with the
# other demos. Runs on hart 0 (QEMU boots the ELF straight into
# M-mode with -bios none): records the boot medeleg, runs the
# write/readback triple on medeleg bit 2 (illegal-instruction
# delegation) requiring the bit admitted, installs an M-mode trap
# handler that only records (no M-mode trap is expected), installs
# an S-mode handler that records scause/sepc/stval at entry, raises
# a done flag, skips the 4-byte illegal word, and sret's, opens the
# whole address space to S-mode with one PMP NAPOT entry, clears
# mie and mstatus.MIE (no interrupt of either kind can fire), then
# drops to S-mode. The S-mode payload executes the word
# 0xffffffff at a labeled site, requires exactly one S-mode trap
# with scause == 2, sepc at the word, and stval equal to the
# executed encoding, then takes a quiet window and prints the
# verdict. On PASS the machine shuts down via the virt test-device
# finisher so the QEMU process exit code (0) reflects the verdict;
# on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in STVC_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
STVC_SRCS := src/boot.S src/uart.c \
            src/stval-illegal-capture/stvc_trap.S src/stval-illegal-capture/stvc_main.c
STVC_OBJS := $(STVC_SRCS:.c=.o)
STVC_OBJS := $(STVC_OBJS:.S=.o)

stval-illegal-capture.elf: $(STVC_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(STVC_OBJS)

# Run the stval-illegal-capture module under QEMU.
run-stval-illegal-capture: stval-illegal-capture.elf
	$(QEMU) -machine virt -nographic -bios none -kernel stval-illegal-capture.elf

# stval-ecall-capture module (backlog riscv stval-ecall-capture):
# its own binary sharing only boot.S and the UART driver with the
# other demos. Runs on hart 0 (QEMU boots the ELF straight into
# M-mode with -bios none): records the boot medeleg, runs the
# write/readback triple on medeleg bit 8 (U-mode environment-call
# delegation) requiring the bit admitted and nothing else set,
# installs an M-mode trap handler that only records (no M-mode
# trap is expected), installs an S-mode handler that records
# scause/sepc/stval at entry, raises a done flag, skips the 4-byte
# ecall, and sret's, opens the whole address space to lower modes
# with one PMP NAPOT entry, clears mie and mstatus.MIE (no
# interrupt of either kind can fire), then sret's to U-mode with
# sstatus.SPP = 0. The U-mode payload issues one ecall at a
# labeled site, requires exactly one S-mode trap with scause == 8
# and stval == 0 (environment calls report nothing in stval),
# zero M-mode traps, then takes a quiet window and prints the
# verdict. On PASS the machine shuts down via the virt test-device
# finisher so the QEMU process exit code (0) reflects the verdict;
# on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in STEC_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
STEC_SRCS := src/boot.S src/uart.c \
            src/stval-ecall-capture/sec_trap.S src/stval-ecall-capture/sec_main.c
STEC_OBJS := $(STEC_SRCS:.c=.o)
STEC_OBJS := $(STEC_OBJS:.S=.o)

stval-ecall-capture.elf: $(STEC_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(STEC_OBJS)

# Run the stval-ecall-capture module under QEMU.
run-stval-ecall-capture: stval-ecall-capture.elf
	$(QEMU) -machine virt -nographic -bios none -kernel stval-ecall-capture.elf

# PMP NAPOT encode write/readback module (backlog item
# "riscv pmp-napot-encode"): its own binary sharing only boot.S and
# the UART driver with the other demos. M-mode only, no traps
# expected (L stays 0 throughout, so unlocked PMP entries cannot
# restrict M-mode). Records boot pmpaddr0/pmpaddr1/pmpcfg0, writes
# three NAPOT patterns (4 KiB, 64 KiB, 1 MiB) to pmpaddr0 requiring
# exact readback, programs pmpcfg0 entry 0 with A=NAPOT (0x18)
# requiring the A field to read 3 and with 0x1F requiring exact
# readback, then restores all three CSRs to boot values with the
# restore itself checked. Prints the written-vs-readback table and
# an FNV-1a checksum over the recorded values. On PASS the machine
# shuts down via the virt test-device finisher so the QEMU process
# exit code (0) reflects the verdict; on FAIL the hart parks.
# NOTE: src/boot.S must stay first in PNEN_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
PNEN_SRCS := src/boot.S src/uart.c \
            src/pmp-napot-encode/pne_main.c
PNEN_OBJS := $(PNEN_SRCS:.c=.o)
PNEN_OBJS := $(PNEN_OBJS:.S=.o)

pmp-napot-encode.elf: $(PNEN_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(PNEN_OBJS)

# Run the pmp-napot-encode module under QEMU.
run-pmp-napot-encode: pmp-napot-encode.elf
	$(QEMU) -machine virt -nographic -bios none -kernel pmp-napot-encode.elf

# mstatus.FS Clean->Dirty transition module (backlog "riscv
# sstatus-fs-dirty"): its own binary sharing only boot.S and the UART
# driver with the other demos. Records the boot mstatus and requires
# FS == 0 (Off), sets FS to Initial (1) with csrs (an FP instruction
# with FS == Off would raise illegal-instruction), executes one
# fmv.d.x and requires FS == 3 (Dirty), writes a second FP register
# and requires FS to stay 3 (sticky Dirty), then clears FS to 0 and
# requires the full mstatus word to match the boot baseline before
# restoring the baseline exactly. A counting M-mode trap handler is
# installed as a safety net; the run requires its counter to stay 0.
# On PASS it shuts the machine down via the virt test-device
# finisher so the QEMU process exit code (0) reflects the verdict;
# on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in SFD_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
SFD_SRCS := src/boot.S src/uart.c \
            src/sstatus-fs-dirty/sfd_trap.S src/sstatus-fs-dirty/sfd_main.c
SFD_OBJS := $(SFD_SRCS:.c=.o)
SFD_OBJS := $(SFD_OBJS:.S=.o)

sstatus-fs-dirty.elf: $(SFD_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SFD_OBJS)

# Run the sstatus-fs-dirty module under QEMU.
run-sstatus-fs-dirty: sstatus-fs-dirty.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sstatus-fs-dirty.elf

# fflags.NX accrual module (backlog "riscv fflags-nx-inexact"): its own
# binary sharing only boot.S and the UART driver with the other
# demos. Sets mstatus.FS to Initial (1) (an FP instruction with
# FS == Off would raise illegal-instruction), clears fcsr with
# csrw fcsr, x0 (frm=RNE) and requires the readback 0x00, executes
# one inexact double divide 1.0/3.0 as a real in-asm volatile fdiv.d
# loaded with fmv.d.x from the 1.0 and 3.0 bit patterns, and requires
# the fcsr readback to be exactly 0x01 (NX set, no other flag bit
# moved, frm still RNE). Clears fcsr again and requires the readback
# 0x00. Sanity anchor: the quotient read back with fmv.x.d must
# equal the correctly rounded 1/3 double, 0x3FD5555555555555; the
# verdict rests on the fflags checks. A counting M-mode trap
# handler is installed as a safety net; the run requires its counter
# to stay 0. On PASS it shuts the machine down via the virt
# test-device finisher so the QEMU process exit code (0) reflects
# the verdict; on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in FNX_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
FNX_SRCS := src/boot.S src/uart.c \
            src/fflags-nx-inexact/fnx_trap.S src/fflags-nx-inexact/fnx_main.c
FNX_OBJS := $(FNX_SRCS:.c=.o)
FNX_OBJS := $(FNX_OBJS:.S=.o)

fflags-nx-inexact.elf: $(FNX_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FNX_OBJS)

# Run the fflags-nx-inexact module under QEMU.
run-fflags-nx-inexact: fflags-nx-inexact.elf
	$(QEMU) -machine virt -nographic -bios none -kernel fflags-nx-inexact.elf

# fflags.UF accrual module (backlog "riscv fflags-uf-underflow"): its own
# binary sharing only boot.S and the UART driver with the other
# demos. Sets mstatus.FS to Initial (1) (an FP instruction with
# FS == Off would raise illegal-instruction), clears fcsr with
# csrw fcsr, x0 (frm=RNE) and requires the readback 0x00, executes
# one underflowing double multiply DBL_MIN * DBL_MIN as a real
# in-asm volatile fmul.d loaded with fmv.d.x from the 0x0010000000000000
# bit pattern, and requires the fcsr readback to be exactly 0x11
# (UF and NX set, NV/DZ/OF all 0, frm still RNE). The exact product
# 2^-2044 is below emin (-1022), so tiny, and rounds to 0, so
# inexact: tiny plus inexact is the IEEE 754 underflow condition.
# Clears fcsr again and requires the readback 0x00. Sanity anchor:
# the product read back with fmv.x.d must be
# 0x0000000000000000 (underflowed to zero); the verdict rests on
# the fflags checks. A counting M-mode trap handler is installed
# as a safety net; the run requires its counter to stay 0. On PASS
# it shuts the machine down via the virt test-device finisher so
# the QEMU process exit code (0) reflects the verdict; on FAIL it
# parks the hart instead.
# NOTE: src/boot.S must stay first in FFU_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
FFU_SRCS := src/boot.S src/uart.c \
            src/fflags-uf-underflow/ffu_trap.S src/fflags-uf-underflow/ffu_main.c
FFU_OBJS := $(FFU_SRCS:.c=.o)
FFU_OBJS := $(FFU_OBJS:.S=.o)

fflags-uf-underflow.elf: $(FFU_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FFU_OBJS)

# Run the fflags-uf-underflow module under QEMU.
run-fflags-uf-underflow: fflags-uf-underflow.elf
	$(QEMU) -machine virt -nographic -bios none -kernel fflags-uf-underflow.elf

# fflags.OF accrual module (backlog "riscv fflags-of-overflow"): its own
# binary sharing only boot.S and the UART driver with the other
# demos. Sets mstatus.FS to Initial (1) (an FP instruction with
# FS == Off would raise illegal-instruction), clears fcsr with
# csrw fcsr, x0 (frm=RNE) and requires the readback 0x00, executes
# one overflowing double multiply 1e308 * 1e308 as a real in-asm
# volatile fmul.d loaded with fmv.d.x from the 0x7FEFFFFFFFFFFFFF
# bit pattern (the largest finite double), and requires the fcsr
# readback to be exactly 0x5 (OF=bit 2 and NX=bit 0 set, UF/DZ/NV
# all 0, frm still RNE). The exact product, about 1e616, exceeds
# the largest finite double, so the operation overflows; the
# RNE-delivered result is +infinity, which differs from the exact
# finite product, so it is inexact too. Clears fcsr again and
# requires the readback 0x00. Sanity anchor, logged not asserted:
# the product read back with fmv.x.d is 0x7FF0000000000000
# (+infinity); the verdict rests on the fflags checks. A counting
# M-mode trap handler is installed as a safety net; the run
# requires its counter to stay 0. On PASS it shuts the machine
# down via the virt test-device finisher so the QEMU process exit
# code (0) reflects the verdict; on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in FOF_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
FOF_SRCS := src/boot.S src/uart.c \
            src/fflags-of-overflow/fof_trap.S src/fflags-of-overflow/fof_main.c
FOF_OBJS := $(FOF_SRCS:.c=.o)
FOF_OBJS := $(FOF_OBJS:.S=.o)

fflags-of-overflow.elf: $(FOF_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FOF_OBJS)

# Run the fflags-of-overflow module under QEMU.
run-fflags-of-overflow: fflags-of-overflow.elf
	$(QEMU) -machine virt -nographic -bios none -kernel fflags-of-overflow.elf

# fflags.DZ accrual module (backlog "riscv fflags-dz-divide-by-zero"):
# its own binary sharing only boot.S and the UART driver with the
# other demos. Sets mstatus.FS to Initial (1) (an FP instruction
# with FS == Off would raise illegal-instruction), clears fcsr with
# csrw fcsr, x0 (frm=RNE) and requires the readback 0x00, executes
# one divide-by-zero 1.0/0.0 as a real in-asm volatile fdiv.d loaded
# with fmv.d.x from the 0x3FF0000000000000 (1.0) and
# 0x0000000000000000 (+0.0) bit patterns, and requires the fcsr
# readback to be exactly 0x8 (DZ=bit 3 set, NX/UF/OF/NV all 0, frm
# still RNE). Clears fcsr again and requires the readback 0x00.
# Sanity anchor: the quotient read back with fmv.x.d must be
# 0x7FF0000000000000 (+infinity); the verdict rests on the fflags
# checks. A counting M-mode trap handler is installed as a safety
# net; the run requires its counter to stay 0. On PASS it shuts the
# machine down via the virt test-device finisher so the QEMU
# process exit code (0) reflects the verdict; on FAIL it parks the
# hart instead.
# NOTE: src/boot.S must stay first in FDZ_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
FDZ_SRCS := src/boot.S src/uart.c \
            src/fflags-dz-divide-by-zero/fdz_trap.S src/fflags-dz-divide-by-zero/fdz_main.c
FDZ_OBJS := $(FDZ_SRCS:.c=.o)
FDZ_OBJS := $(FDZ_OBJS:.S=.o)

fflags-dz-divide-by-zero.elf: $(FDZ_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FDZ_OBJS)

# Run the fflags-dz-divide-by-zero module under QEMU.
run-fflags-dz-divide-by-zero: fflags-dz-divide-by-zero.elf
	$(QEMU) -machine virt -nographic -bios none -kernel fflags-dz-divide-by-zero.elf

# fflags.NV accrual module (backlog "riscv fflags-nv-invalid"):
# its own binary sharing only boot.S and the UART driver with the
# other demos. Sets mstatus.FS to Initial (1) (an FP instruction
# with FS == Off would raise illegal-instruction), clears fcsr with
# csrw fcsr, x0 (frm=RNE) and requires the readback 0x00, executes
# one invalid divide 0.0/0.0 as a real in-asm volatile fdiv.d loaded
# with fmv.d.x from the 0x0000000000000000 (+0.0) bit pattern, and
# requires the fcsr readback to be exactly 0x10 (NV=bit 4 set,
# NX/UF/OF/DZ all 0, frm still RNE). Clears fcsr again and requires
# the readback 0x00. Sanity anchor: the quotient read back with
# fmv.x.d must be 0x7FF8000000000000 (canonical quiet NaN); the
# verdict rests on the fflags checks. A counting M-mode trap
# handler is installed as a safety net; the run requires its
# counter to stay 0. On PASS it shuts the machine down via the
# virt test-device finisher so the QEMU process exit code (0)
# reflects the verdict; on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in FNV_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
FNV_SRCS := src/boot.S src/uart.c \
            src/fflags-nv-invalid/fnv_trap.S src/fflags-nv-invalid/fnv_main.c
FNV_OBJS := $(FNV_SRCS:.c=.o)
FNV_OBJS := $(FNV_OBJS:.S=.o)

fflags-nv-invalid.elf: $(FNV_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FNV_OBJS)

# Run the fflags-nv-invalid module under QEMU.
run-fflags-nv-invalid: fflags-nv-invalid.elf
	$(QEMU) -machine virt -nographic -bios none -kernel fflags-nv-invalid.elf

# sie.SSIE gate module (backlog item "riscv
# sie-ssip-clear-suppresses"): its own binary sharing only boot.S and
# the UART driver with the other demos. M-mode delegates the
# supervisor software interrupt via mideleg bit 1 (readback: bit 1
# takes, bit 9 stays clear), pends SSIP from M-mode with csrs mip
# (S-mode sip writes to bit 1 are dropped on this hart), clears sie
# entirely, and srets to S-mode with sstatus.SIE set. Phase 1 polls a
# bounded window with SSIE clear and requires sip.SSIP to read 1
# throughout with zero traps. Phase 2 sets SSIE inside a labeled wait
# loop and requires exactly one S-mode trap with scause
# 0x8000000000000001 and sepc inside the loop; the handler records
# scause/sepc, the sip value at entry, and clears SSIP. Phase 3 runs a
# bounded quiet window with both gates open and requires no
# re-delivery. A checks/mismatches summary and an FNV-1a checksum over
# the verdict values are printed. On PASS it shuts the machine down
# via the virt test-device finisher so the QEMU process exit code (0)
# reflects the verdict; on FAIL it parks instead.
# NOTE: src/boot.S must stay first in SSCS_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
SSCS_SRCS := src/boot.S src/uart.c \
             src/sie-ssip-clear-suppresses/ssc_trap.S src/sie-ssip-clear-suppresses/ssc_main.c
SSCS_OBJS := $(SSCS_SRCS:.c=.o)
SSCS_OBJS := $(SSCS_OBJS:.S=.o)

sie-ssip-clear-suppresses.elf: $(SSCS_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SSCS_OBJS)

# Run the sie.SSIE gate module under QEMU.
run-sie-ssip-clear-suppresses: sie-ssip-clear-suppresses.elf
	$(QEMU) -machine virt -nographic -bios none -kernel sie-ssip-clear-suppresses.elf

# fcsr.frm rounding-mode write/readback module (backlog "riscv
# frm-rounding-write"): its own binary sharing only boot.S and the UART
# driver with the other demos. In M-mode: installs the counting trap
# handler safety net (requires 0 traps), clears mstatus.MIE and asserts
# mie == 0, requires misa F+D (the sanity anchor executes fdiv.d),
# requires boot mstatus.FS == Off and sets FS to Initial before any FP
# write. Then for each of the five rounding modes {RNE, RTZ, RDN, RUP,
# RMM} it writes fcsr = (mode << 5) with fflags 0 via csrw fcsr, reads
# fcsr back, and requires the frm field to equal the written mode with
# the fflags field still 0. The written-vs-readback frm pairs are
# printed every run. A sanity anchor (logged only) divides 1.0/3.0
# under RDN vs RUP via fdiv.d and checks the quotient bit patterns
# are 0x3FD5555555555555 vs 0x3FD5555555555556, proving the written
# mode steers hardware rounding. Finally it restores frm=RNE and
# requires the full fcsr word to read back 0x00. A 64-bit FNV-1a
# checksum over the logged measurement words is printed. On PASS it
# shuts the machine down via the virt test-device finisher so the
# QEMU process exit code (0) reflects the verdict; on FAIL it parks
# the hart instead.
# NOTE: src/boot.S must stay first in FRW_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
FRW_SRCS := src/boot.S src/uart.c \
            src/frm-rounding-write/frw_trap.S src/frm-rounding-write/frw_main.c
FRW_OBJS := $(FRW_SRCS:.c=.o)
FRW_OBJS := $(FRW_OBJS:.S=.o)

frm-rounding-write.elf: $(FRW_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FRW_OBJS)

# Run the frm-rounding-write module under QEMU.
run-frm-rounding-write: frm-rounding-write.elf
	$(QEMU) -machine virt -nographic -bios none -kernel frm-rounding-write.elf

# fcsr fflags/frm field-independence module (backlog "riscv
# fcsr-field-independence"): its own binary sharing only boot.S
# and the UART driver with the other demos. Sets mstatus.FS to
# Initial (1) (an FP instruction with FS == Off would raise
# illegal-instruction), clears fcsr with csrw fcsr, x0 (frm=RNE)
# and requires the readback 0x00, runs one inexact fdiv.d
# 1.0/3.0 as a real volatile in-asm instruction (operands fed as
# bit patterns through fmv.d.x) and requires the readback 0x01
# (NX set, frm still RNE), then csrs fcsr, (3<<5) to set frm=RUP
# and requires the readback 0x61 (accrued NX preserved, frm==3),
# then csrc fcsr, 0x1f to clear the fflags field and requires
# the readback 0x60 (frm preserved at RUP, flags all 0), then
# csrw fcsr, x0 again and requires the readback 0x00. Every
# step's readback is required exact, so a write to one field
# disturbing the other fails the run. A counting M-mode trap
# handler is installed as a safety net; the run requires its
# counter to stay 0. On PASS it shuts the machine down via the
# virt test-device finisher so the QEMU process exit code (0)
# reflects the verdict; on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in FFI_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
FFI_SRCS := src/boot.S src/uart.c \
            src/fcsr-field-independence/ffi_trap.S src/fcsr-field-independence/ffi_main.c
FFI_OBJS := $(FFI_SRCS:.c=.o)
FFI_OBJS := $(FFI_OBJS:.S=.o)

fcsr-field-independence.elf: $(FFI_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FFI_OBJS)

# Run the fcsr-field-independence module under QEMU.
run-fcsr-field-independence: fcsr-field-independence.elf fcsr-frm-roundup.elf
	$(QEMU) -machine virt -nographic -bios none -kernel fcsr-field-independence.elf fcsr-frm-roundup.elf

# fcsr frm rounding-direction module (backlog "riscv fcsr-frm-roundup"):
# its own binary sharing only boot.S and the UART driver with the
# other demos. Sets mstatus.FS to Initial (1) (an FP instruction
# with FS == Off would raise illegal-instruction), then runs five
# trials, each: csrw fcsr, (frm<<5) to set the frm field and clear
# fflags, one volatile in-asm FP instruction with rm=DYN so the
# hardware rounds with the frm field, then reads the result bits
# (fmv.x.d) and fcsr. Vector 1 is fadd.d(1.0, 2^-53), whose exact
# sum is exactly halfway between 0x3FF0000000000000 and
# 0x3FF0000000000001: RNE expects 0x3FF0000000000000 (ties to
# even), RNZ expects 0x3FF0000000000000 (toward zero), RUP
# expects 0x3FF0000000000001 (toward +inf). Vector 2 is
# fdiv.d(1.0, 3.0): RNE expects 0x3FD5555555555555 (the nearer
# candidate), RUP expects 0x3FD5555555555556 (the next candidate
# up). Every trial asserts the result bits against the
# analytically derived expectation, fflags == 0x01 (NX only), and
# the frm field still reading the trial's mode. A counting M-mode
# trap handler is installed as a safety net; the run requires its
# counter to stay 0, and a final FS readback sanity check requires
# FS != Off after the FP writes. On PASS it shuts the machine
# down via the virt test-device finisher so the QEMU process exit
# code (0) reflects the verdict; on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in FFR_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
FFR_SRCS := src/boot.S src/uart.c \
            src/fcsr-frm-roundup/fru_trap.S src/fcsr-frm-roundup/fru_main.c
FFR_OBJS := $(FFR_SRCS:.c=.o)
FFR_OBJS := $(FFR_OBJS:.S=.o)

fcsr-frm-roundup.elf: $(FFR_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FFR_OBJS)

# Run the fcsr frm rounding-direction module under QEMU.
run-fcsr-frm-roundup: fcsr-frm-roundup.elf
	$(QEMU) -machine virt -nographic -bios none -kernel fcsr-frm-roundup.elf

# fcsr frm round-toward-minus-inf module (backlog "riscv fcsr-frm-rounddn"):
# its own binary sharing only boot.S and the UART driver with the
# other demos. Sets mstatus.FS to Initial (1) (an FP instruction
# with FS == Off would raise illegal-instruction), then runs two
# trials, each: csrw fcsr to set the frm field and clear fflags
# (0x40 for RDN, 0x00 for RNE), require the exact readback, one
# volatile in-asm fsub.d with rm=DYN so the hardware rounds with
# the frm field, then reads the result bits (fmv.x.d) and fcsr.
# The subtraction is (1 + 3*2^-52) - 2^-55, whose exact difference
# is 1 + 2.875*2^-52: strictly between the adjacent doubles
# 0x3FF0000000000002 and 0x3FF0000000000003 and closer to the
# upper one (7/8 ulp below, 1/8 ulp above). RDN expects the lower
# neighbor 0x3FF0000000000002; RNE expects the nearer, upper
# neighbor 0x3FF0000000000003. Every trial asserts the result
# bits against the analytically derived expectation, fflags ==
# 0x01 (NX only), and the frm field still reading the trial's
# mode; the RNE trial additionally asserts q_rdn == q_rne - 1
# and q_rdn < q_rne. fcsr is restored to its exact boot value at
# the end. A counting M-mode trap handler is installed as a
# safety net; the run requires its counter to stay 0, and a
# final FS readback sanity check requires FS != Off after the FP
# writes. On PASS it shuts the machine down via the virt
# test-device finisher so the QEMU process exit code (0) reflects
# the verdict; on FAIL it parks the hart instead.
# NOTE: src/boot.S must stay first in FDN_SRCS so _start lands at
# 0x80000000, the address QEMU's -kernel loader starts at.
FDN_SRCS := src/boot.S src/uart.c \
            src/fcsr-frm-rounddn/frd_trap.S src/fcsr-frm-rounddn/frd_main.c
FDN_OBJS := $(FDN_SRCS:.c=.o)
FDN_OBJS := $(FDN_OBJS:.S=.o)

fcsr-frm-rounddn.elf: $(FDN_OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FDN_OBJS)

# Run the fcsr frm round-toward-minus-inf module under QEMU.
run-fcsr-frm-rounddn: fcsr-frm-rounddn.elf
	$(QEMU) -machine virt -nographic -bios none -kernel fcsr-frm-rounddn.elf
