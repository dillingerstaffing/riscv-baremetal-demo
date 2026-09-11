#!/bin/sh
# build.sh: build mtvec-direct-vectoring.elf with the same flags as the repo Makefile.
# Run from the repo root: sh src/mtvec-direct-vectoring/build.sh
set -e
CROSS=riscv64-unknown-elf-
CC=${CROSS}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
SRCS="src/boot.S src/uart.c src/mtvec-direct-vectoring/dvm_trap.S src/mtvec-direct-vectoring/dvm_main.c"
$CC $CFLAGS -T link.ld -o mtvec-direct-vectoring.elf $SRCS
echo "built mtvec-direct-vectoring.elf"
