#!/bin/sh
# build.sh: build misa-mxl-warl.elf with the same flags as the repo Makefile.
# Run from the repo root: sh src/misa-mxl-warl/build.sh
set -e
CROSS=riscv64-unknown-elf-
CC=${CROSS}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
SRCS="src/boot.S src/uart.c src/misa-mxl-warl/mxl_trap.S src/misa-mxl-warl/mxl_main.c"
$CC $CFLAGS -T link.ld -o misa-mxl-warl.elf $SRCS
echo "built misa-mxl-warl.elf"
