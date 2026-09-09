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

all: demo.elf preempt.elf virtio-blk.elf smp.elf shell.elf uart-baud.elf smode.elf smode-mbase.elf pmp.elf wfi-latency.elf mal.elf plic.elf mtimecmp.elf sv39.elf csr.elf ecall.elf counter-alias.elf amo.elf

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

# Run the misaligned-access experiment under QEMU.
run-mal: mal.elf
	$(QEMU) -machine virt -nographic -bios none -kernel mal.elf

# Run the misaligned LR/SC experiment under QEMU.
run-amo: amo.elf
	$(QEMU) -machine virt -nographic -bios none -kernel amo.elf

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

clean:
	rm -f $(OBJS) $(PREEMPT_OBJS) $(VIRTIO_OBJS) $(SMP_OBJS) $(SHELL_OBJS) $(UARTBAUD_OBJS) $(SMODE_S_OBJS) $(SMODE_M_OBJS) $(PMP_OBJS) $(WFI_OBJS) $(MIS_OBJS) $(PLIC_OBJS) $(MT_OBJS) $(SV39_OBJS) $(ECALL_OBJS) $(CA_OBJS) $(AMO_OBJS) demo.elf preempt.elf virtio-blk.elf smp.elf shell.elf uart-baud.elf smode.elf smode-mbase.elf pmp.elf wfi-latency.elf mal.elf plic.elf mtimecmp.elf sv39.elf ecall.elf counter-alias.elf amo.elf

.PHONY: all run clean
