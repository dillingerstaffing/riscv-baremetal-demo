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

all: demo.elf preempt.elf virtio-blk.elf smp.elf shell.elf uart-baud.elf smode.elf smode-mbase.elf

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

clean:
	rm -f $(OBJS) $(PREEMPT_OBJS) $(VIRTIO_OBJS) $(SMP_OBJS) $(SHELL_OBJS) $(UARTBAUD_OBJS) $(SMODE_S_OBJS) $(SMODE_M_OBJS) demo.elf preempt.elf virtio-blk.elf smp.elf shell.elf uart-baud.elf smode.elf smode-mbase.elf

.PHONY: all run clean
