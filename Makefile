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

all: demo.elf preempt.elf virtio-blk.elf smp.elf shell.elf uart-baud.elf smode.elf smode-mbase.elf pmp.elf wfi-latency.elf mal.elf plic.elf mtimecmp.elf sv39.elf csr.elf ecall.elf counter-alias.elf amo.elf umode.elf msip.elf mtvec-vectored.elf cycmon.elf fs-check.elf medeleg-mask.elf wfi-resume-pc.elf lrsc-histogram.elf pmp-tor.elf mpp-encoding.elf mcycle-write.elf mip-msip.elf mie-global.elf sip-ssip.elf sc-fail.elf mret-no-restore.elf stvec-direct.elf mepc-resume-skip.elf sepc-resume-skip.elf pmp-napot-size.elf satp-asid.elf satp-bare.elf mtval-fault-address.elf mcounteren.elf cycle-read-latency.elf mtimecmp-oneshot.elf stimecmp-one-shot.elf mie-stie.elf mcause-warl.elf mideleg-route.elf sepc-warl.elf scause-bit.elf mideleg-warl.elf mtvec-mode0-direct.elf pmp-lock-bit.elf

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

clean:
	rm -f $(OBJS) $(PREEMPT_OBJS) $(VIRTIO_OBJS) $(SMP_OBJS) $(SHELL_OBJS) $(UARTBAUD_OBJS) $(SMODE_S_OBJS) $(SMODE_M_OBJS) $(PMP_OBJS) $(WFI_OBJS) $(MIS_OBJS) $(PLIC_OBJS) $(MT_OBJS) $(SV39_OBJS) $(ECALL_OBJS) $(CA_OBJS) $(AMO_OBJS) $(UMODE_OBJS) $(MSIP_OBJS) $(MTV_OBJS) $(CYCMON_OBJS) $(FSCHECK_OBJS) $(MDEL_OBJS) $(WFIRPC_OBJS) $(PMPTOR_OBJS) $(MPPENC_OBJS) $(MCW_OBJS) $(MMSP_OBJS) $(MIG_OBJS) $(MNR_OBJS) $(SVD_OBJS) $(MRS_OBJS) $(SS_OBJS) $(PNS_OBJS) $(SATPA_OBJS) $(SATPB_OBJS) $(MFA_OBJS) $(MCE_OBJS) $(CRL_OBJS) $(MTOS_OBJS) $(STOS_OBJS) $(STIE_OBJS) $(MCA_OBJS) $(MIDR_OBJS) $(SVV_OBJS) $(SCB_OBJS) $(MIDW_OBJS) $(D0_OBJS) $(PLB_OBJS) demo.elf preempt.elf virtio-blk.elf smp.elf shell.elf uart-baud.elf smode.elf smode-mbase.elf pmp.elf wfi-latency.elf mal.elf plic.elf mtimecmp.elf sv39.elf ecall.elf counter-alias.elf amo.elf umode.elf msip.elf mtvec-vectored.elf cycmon.elf fs-check.elf medeleg-mask.elf wfi-resume-pc.elf pmp-tor.elf mpp-encoding.elf mcycle-write.elf mip-msip.elf mie-global.elf sip-ssip.elf mret-no-restore.elf stvec-direct.elf mepc-resume-skip.elf sepc-resume-skip.elf pmp-napot-size.elf mtval-fault-address.elf mcounteren.elf cycle-read-latency.elf mtimecmp-oneshot.elf stimecmp-one-shot.elf mie-stie.elf mcause-warl.elf mideleg-route.elf stvec-vectored.elf sepc-warl.elf scause-bit.elf mideleg-warl.elf pmp-lock-bit.elf
.PHONY: all run clean
