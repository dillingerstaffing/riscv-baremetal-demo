#!/bin/sh
# build.sh: build the stval-warl-probe module.
# Same flags as the repo Makefile pattern; the Makefile itself is
# not modified. Run from the module directory. The full build log
# is captured in bench-logs/build.log.
set -e
CC=${CROSS:-riscv64-unknown-elf-}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
  -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
cd "$(dirname "$0")"
mkdir -p bench-logs
{
$CC $CFLAGS -c ../boot.S -o boot.o
$CC $CFLAGS -c ../uart.c -o uart.o
$CC $CFLAGS -c stv_main.c -o stv_main.o
$CC $CFLAGS -c stv_strap.S -o stv_strap.o
$CC $CFLAGS -c stv_mtrap.S -o stv_mtrap.o
$CC $CFLAGS -T ../../link.ld -o stval-warl-probe.elf \
  boot.o uart.o stv_main.o stv_strap.o stv_mtrap.o
echo "built stval-warl-probe.elf"
} > bench-logs/build.log 2>&1
cat bench-logs/build.log
