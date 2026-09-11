#!/bin/sh
# build.sh: build the mcounteren-cy-gate module.
# Same flags as the repo Makefile pattern; the Makefile itself is
# not modified. Run from the module directory.
set -e
CC=${CROSS:-riscv64-unknown-elf-}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
  -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
cd "$(dirname "$0")"
$CC $CFLAGS -c ../boot.S -o boot.o
$CC $CFLAGS -c ../uart.c -o uart.o
$CC $CFLAGS -c mcg_main.c -o mcg_main.o
$CC $CFLAGS -c mcg_strap.S -o mcg_strap.o
$CC $CFLAGS -c mcg_mtrap.S -o mcg_mtrap.o
$CC $CFLAGS -T ../../link.ld -o mcounteren-cy-gate.elf \
  boot.o uart.o mcg_main.o mcg_strap.o mcg_mtrap.o
echo "built mcounteren-cy-gate.elf"
