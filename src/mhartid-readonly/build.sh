#!/bin/sh
# build.sh: build mhartid-readonly.elf with the same flags as the repo Makefile.
# Run from the repo root: sh src/mhartid-readonly/build.sh
set -e
CROSS=riscv64-unknown-elf-
CC=${CROSS}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
SRCS="src/boot.S src/uart.c src/mhartid-readonly/mhr_trap.S src/mhartid-readonly/mhr_main.c"
$CC $CFLAGS -T link.ld -o mhartid-readonly.elf $SRCS
echo "built mhartid-readonly.elf"
