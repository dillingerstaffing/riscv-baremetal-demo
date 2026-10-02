#!/bin/sh
# build.sh: build mhartid-u-mode-read.elf with the same flags as the repo Makefile.
# Run from the repo root: sh src/mhartid-u-mode-read/build.sh
set -e
CROSS=riscv64-unknown-elf-
CC=${CROSS}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
SRCS="src/boot.S src/uart.c src/mhartid-u-mode-read/mhu_mtrap.S src/mhartid-u-mode-read/mhu_strap.S src/mhartid-u-mode-read/mhu_main.c"
$CC $CFLAGS -T link.ld -o mhartid-u-mode-read.elf $SRCS
echo "built mhartid-u-mode-read.elf"
