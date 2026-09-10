#!/bin/sh
# build.sh: build misa-readonly.elf with the same flags as the repo Makefile.
# Run from the repo root: sh src/misa-readonly/build.sh
set -e
CROSS=riscv64-unknown-elf-
CC=${CROSS}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
SRCS="src/boot.S src/uart.c src/misa-readonly/misa_trap.S src/misa-readonly/misa_main.c"
$CC $CFLAGS -T link.ld -o misa-readonly.elf $SRCS
echo "built misa-readonly.elf"
