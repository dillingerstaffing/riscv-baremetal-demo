#!/bin/sh
# build.sh: build the cbo-zero-gate module from the repo root, using the
# same flags as the repo Makefile pattern. Run from the module directory;
# the log is captured to bench-logs/build.log.
#
# Toolchain: Ubuntu gcc-riscv64-unknown-elf 13.2.0 (binutils 2.42) at
# ~/workspace/toolchains/ubuntu-rv64. The xPack 15.2.0 toolchain's ld
# mis-links these objects (then segfaults), so it is not used.
#
# Link order matters: boot.o MUST come first so that _start lands at
# 0x80000000, the address QEMU's -kernel loader jumps to (it does not
# honor a nonzero ELF entry offset; a trap.o-first link hangs
# silently). This matches every other module's build order.
set -e
TC=${TOOLCHAIN:-/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin}
CC=$TC/riscv64-unknown-elf-gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
  -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
cd "$(dirname "$0")/../.."
$CC $CFLAGS -c src/boot.S -o src/boot_czg.o
$CC $CFLAGS -c src/uart.c -o src/uart_czg.o
$CC $CFLAGS -c src/cbo-zero-gate/czg_main.c -o src/cbo-zero-gate/czg_main.o
$CC $CFLAGS -c src/cbo-zero-gate/czg_trap.S -o src/cbo-zero-gate/czg_trap.o
$CC $CFLAGS -T link.ld -o cbo-zero-gate.elf \
  src/boot_czg.o src/uart_czg.o \
  src/cbo-zero-gate/czg_main.o src/cbo-zero-gate/czg_trap.o
echo "built cbo-zero-gate.elf"
