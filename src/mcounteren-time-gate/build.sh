#!/bin/sh
# build.sh: build the mcounteren-time-gate module.
# Same flags as the repo Makefile pattern; the Makefile itself is
# not modified. Run from the module directory.
set -e
CC=${CROSS:-riscv64-unknown-elf-}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
  -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
cd "$(dirname "$0")"
$CC $CFLAGS -c ../boot.S -o boot.o
$CC $CFLAGS -c ../uart.c -o uart.o
$CC $CFLAGS -c mtg_main.c -o mtg_main.o
$CC $CFLAGS -c mtg_strap.S -o mtg_strap.o
$CC $CFLAGS -c mtg_mtrap.S -o mtg_mtrap.o
$CC $CFLAGS -T ../../link.ld -o mcounteren-time-gate.elf \
  boot.o uart.o mtg_main.o mtg_strap.o mtg_mtrap.o
echo "built mcounteren-time-gate.elf"
