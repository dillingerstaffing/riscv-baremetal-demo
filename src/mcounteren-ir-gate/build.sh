#!/bin/sh
# build.sh: build the mcounteren-ir-gate module.
# Same flags as the repo Makefile pattern; the Makefile itself is
# not modified. Run from the module directory.
set -e
CC=${CROSS:-riscv64-unknown-elf-}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
  -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
cd "$(dirname "$0")"
$CC $CFLAGS -c ../boot.S -o boot.o
$CC $CFLAGS -c ../uart.c -o uart.o
$CC $CFLAGS -c mir_main.c -o mir_main.o
$CC $CFLAGS -c mir_strap.S -o mir_strap.o
$CC $CFLAGS -c mir_mtrap.S -o mir_mtrap.o
$CC $CFLAGS -T ../../link.ld -o mcounteren-ir-gate.elf \
  boot.o uart.o mir_main.o mir_strap.o mir_mtrap.o
echo "built mcounteren-ir-gate.elf"
