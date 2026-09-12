#!/bin/sh
# build.sh: build the sstatus-fs-off-fp-trap module from the repo root,
# using the same flags as the repo Makefile pattern. Run from the module
# directory; the log is captured to bench-logs/build.log.
#
# Toolchain: the Ubuntu gcc-riscv64-unknown-elf 13.2.0 package extracted
# under ~/workspace/toolchains/ubuntu-rv64 (do NOT use the xPack 15.2.0
# toolchain, its ld mis-links user objects). Link order matters:
# boot.o MUST come first so that _start lands at 0x80000000, the
# address QEMU's -kernel loader jumps to (it does not honor a nonzero
# ELF entry offset; a trap.o-first link hangs silently). This matches
# every other module's Makefile SRCS order.
set -e
CC=${CROSS:-/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
  -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
cd "$(dirname "$0")/../.."
$CC $CFLAGS -c src/boot.S -o src/boot_sfo.o
$CC $CFLAGS -c src/uart.c -o src/uart_sfo.o
$CC $CFLAGS -c src/sstatus-fs-off-fp-trap/sfo_main.c -o src/sstatus-fs-off-fp-trap/sfo_main.o
$CC $CFLAGS -c src/sstatus-fs-off-fp-trap/sfo_trap.S -o src/sstatus-fs-off-fp-trap/sfo_trap.o
$CC $CFLAGS -T link.ld -o sstatus-fs-off-fp-trap.elf \
  src/boot_sfo.o src/uart_sfo.o \
  src/sstatus-fs-off-fp-trap/sfo_main.o src/sstatus-fs-off-fp-trap/sfo_trap.o
echo "built sstatus-fs-off-fp-trap.elf"
