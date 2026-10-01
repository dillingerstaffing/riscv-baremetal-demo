#!/bin/sh
# build.sh: build mhartid-s-mode-read.elf with the same flags as the repo Makefile.
# Run from the repo root: sh src/mhartid-s-mode-read/build.sh
set -e
CROSS=riscv64-unknown-elf-
CC=${CROSS}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
SRCS="src/boot.S src/uart.c src/mhartid-s-mode-read/mhs_mtrap.S src/mhartid-s-mode-read/mhs_strap.S src/mhartid-s-mode-read/mhs_main.c"
$CC $CFLAGS -T link.ld -o mhartid-s-mode-read.elf $SRCS
echo "built mhartid-s-mode-read.elf"
