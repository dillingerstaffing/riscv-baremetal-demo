#!/bin/sh
# build.sh: build the menvcfg-cbze-write module from the repo root, using the
# same flags as the repo Makefile pattern. Run from the module directory;
# the log is captured to bench-logs/build.log.
#
# Link order matters: boot.o MUST come first so that _start lands at
# 0x80000000, the address QEMU's -kernel loader jumps to (it does not
# honor a nonzero ELF entry offset; a trap.o-first link hangs
# silently). This matches every other module's Makefile SRCS order.
set -e
CC=${CROSS:-/home/hatch/workspace/toolchains/compat-bin/riscv64-unknown-elf-}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
  -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
cd "$(dirname "$0")/../.."
$CC $CFLAGS -c src/boot.S -o src/boot_mcw.o
$CC $CFLAGS -c src/uart.c -o src/uart_mcw.o
$CC $CFLAGS -c src/menvcfg-cbze-write/mcw_main.c -o src/menvcfg-cbze-write/mcw_main.o
$CC $CFLAGS -c src/menvcfg-cbze-write/mcw_trap.S -o src/menvcfg-cbze-write/mcw_trap.o
$CC $CFLAGS -T link.ld -o menvcfg-cbze-write.elf \
  src/boot_mcw.o src/uart_mcw.o \
  src/menvcfg-cbze-write/mcw_main.o src/menvcfg-cbze-write/mcw_trap.o
echo "built menvcfg-cbze-write.elf"
