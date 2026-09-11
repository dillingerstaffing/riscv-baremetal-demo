#!/bin/sh
# build.sh: build the frm-rdn-vs-rup-div module from the repo root.
# Run from the module directory; the log is captured to
# bench-logs/build.log. Each compile/link command is echoed before it
# runs so the log is a faithful build record.
#
# Toolchain: the Ubuntu gcc-riscv64-unknown-elf 13.2.0 / binutils 2.42
# packages (the xPack 15.2.0 ld mis-links and segfaults on some
# objects; see the workspace AGENTS.md xv6 toolchain note).
#
# Link order matters: boot.o MUST come first so that _start lands at
# 0x80000000, the address QEMU's -kernel loader jumps to (it does not
# honor a nonzero ELF entry offset; a trap.o-first link hangs
# silently). This matches every other module's link order.
set -e
CC=${CROSS:-/home/hatch/workspace/toolchains/ubuntu-rv64/usr/bin/riscv64-unknown-elf-}gcc
CFLAGS="-Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles \
  -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany"
cd "$(dirname "$0")/../.."
run() {
    echo "$*"
    "$@"
}
run $CC $CFLAGS -c src/boot.S -o src/boot.o
run $CC $CFLAGS -c src/uart.c -o src/uart.o
run $CC $CFLAGS -c src/frm-rdn-vs-rup-div/rnd_main.c -o src/frm-rdn-vs-rup-div/rnd_main.o
run $CC $CFLAGS -c src/frm-rdn-vs-rup-div/rnd_trap.S -o src/frm-rdn-vs-rup-div/rnd_trap.o
run $CC $CFLAGS -T link.ld -o frm-rdn-vs-rup-div.elf \
  src/boot.o src/uart.o \
  src/frm-rdn-vs-rup-div/rnd_trap.o src/frm-rdn-vs-rup-div/rnd_main.o
echo "built frm-rdn-vs-rup-div.elf"
