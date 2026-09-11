#!/bin/sh
# build.sh: build the mstatus-tw-trap module.
# Same flags as the repo Makefile pattern; the Makefile itself is
# not modified. Run from the module directory.
set -e
CC=${CROSS:-riscv64-unknown-elf-}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
  -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
cd "$(dirname "$0")"
$CC $CFLAGS -c ../boot.S -o boot.o
$CC $CFLAGS -c ../uart.c -o uart.o
$CC $CFLAGS -c tw_main.c -o tw_main.o
$CC $CFLAGS -c tw_strap.S -o tw_strap.o
$CC $CFLAGS -c tw_mtrap.S -o tw_mtrap.o
$CC $CFLAGS -T ../../link.ld -o mstatus-tw-trap.elf \
  boot.o uart.o tw_main.o tw_strap.o tw_mtrap.o
echo "built mstatus-tw-trap.elf"
