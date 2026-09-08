# Makefile for riscv-baremetal-demo.
# Requires: riscv64-unknown-elf-gcc and qemu-system-riscv64
# (Debian/Ubuntu: gcc-riscv64-unknown-elf and qemu-system-misc).

CROSS   ?= riscv64-unknown-elf-
CC      := $(CROSS)gcc

CFLAGS  := -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
           -march=rv64imac -mabi=lp64 -mcmodel=medany
LDFLAGS := -T link.ld

SRCS := src/boot.S src/switch.S src/uart.c src/sched.c src/tasks.c src/main.c
OBJS := $(SRCS:.c=.o)
OBJS := $(OBJS:.S=.o)

all: demo.elf

demo.elf: $(OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

# Run under QEMU. -bios none skips firmware so we boot straight into _start
# in M-mode; -nographic wires the virt UART to the terminal.
run: demo.elf
	qemu-system-riscv64 -machine virt -nographic -bios none -kernel demo.elf

clean:
	rm -f $(OBJS) demo.elf

.PHONY: all run clean
