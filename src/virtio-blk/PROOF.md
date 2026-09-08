# PROOF: virtio-blk block driver on QEMU's virt board

## What was built

A new module, `src/virtio-blk/`, in this repo, built as its own binary
`virtio-blk.elf` (shares only `boot.S` and the UART driver with the other
demos; `demo.elf`, `preempt.elf`, and `smp.elf` are untouched and still
build).

- `virtio.h`: the virtio-mmio register map (offsets, status bits, feature
  bits, device ids) and the split-virtqueue descriptor/used-element
  layouts, with a compile-time assertion that one descriptor is exactly
  16 bytes.
- `virtio.c`: the transport driver. It scans the 32 virtio-mmio slots at
  `0x10001000`+`n`*`0x1000` for a block device (device id 2), runs the
  spec's device status sequence (reset, ACKNOWLEDGE, DRIVER, feature
  negotiation, FEATURES_OK with read-back confirmation, DRIVER_OK), and
  manages one split virtqueue in RAM: descriptor table at queue base,
  available ring after it, used ring on a 4096-byte boundary. Which queue
  registers are used is negotiated, not assumed: if the device offers
  `VIRTIO_F_VERSION_1` the driver programs the 1.x registers
  (QueueDesc/QueueDriver/QueueDevice addresses plus QueueReady);
  otherwise it uses the legacy registers (GuestPageSize, QueueNum,
  QueueAlign, QueuePFN). The driver polls the used ring with `fence`
  barriers; no interrupts are used.
- `blk.h`/`blk.c`: block read/write on the transport. Each request is a
  3-descriptor chain: 16-byte header (type, zeroed second word, sector),
  512-byte data buffer, 1-byte status written back by the device.
  `VIRTIO_BLK_T_OUT`/`VIRTIO_BLK_T_IN` select direction; a non-zero
  status byte is reported as an error.
- `bmain.c`: bring-up and verification. Reads the 64-bit sector count
  from config space, writes a deterministic 512-byte pattern
  (`byte[i] = i ^ (i >> 8) ^ 0x5a`) to sector 0, reads it back into a
  buffer pre-poisoned with `0xaa`, and compares all 512 bytes. It prints
  FNV-1a checksums of the written and read buffers and the mismatch
  count.

## Build log

Toolchain: xPack `riscv-none-elf-gcc` 15.2.0
(`~/workspace/toolchains/xpack-riscv-none-elf-gcc-15.2.0-1`),
`make virtio-blk.elf CROSS=riscv-none-elf-`. Zero errors, zero compiler
warnings:

```
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/virtio-blk/virtio.c -o src/virtio-blk/virtio.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/virtio-blk/blk.c -o src/virtio-blk/blk.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -c src/virtio-blk/bmain.c -o src/virtio-blk/bmain.o
riscv-none-elf-gcc -Wall -Wextra -O2 -ffreestanding -nostdlib -nostartfiles -no-pie -fno-pie -fno-pic -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -T link.ld -o virtio-blk.elf src/boot.o src/uart.o src/virtio-blk/virtio.o src/virtio-blk/blk.o src/virtio-blk/bmain.o
```

## QEMU run output

`qemu-system-riscv64` 8.2.2 (workspace-local `qemu-rv64` wrapper),
`-machine virt -nographic -bios none -kernel virtio-blk.elf`, with a
16 MiB zeroed raw image attached as
`-drive file=disk.img,if=none,format=raw,id=hd0 -device
virtio-blk-device,drive=hd0`. QEMU was stopped with `timeout` after the
demo halted:

```
/home/hatch/workspace/qemu/qemu-rv64 -machine virt -nographic -bios none -kernel virtio-blk.elf \
	-drive file=disk.img,if=none,format=raw,id=hd0 \
	-device virtio-blk-device,drive=hd0
========================================
virtio-blk demo: block driver, sector write/read
========================================

virtio: mmio slot 0 (0x10001000): device id 0, version 1
virtio: mmio slot 1 (0x10002000): device id 0, version 1
virtio: mmio slot 2 (0x10003000): device id 0, version 1
virtio: mmio slot 3 (0x10004000): device id 0, version 1
virtio: mmio slot 4 (0x10005000): device id 0, version 1
virtio: mmio slot 5 (0x10006000): device id 0, version 1
virtio: mmio slot 6 (0x10007000): device id 0, version 1
virtio: mmio slot 7 (0x10008000): device id 2, version 1
virtio: device features lo=0x31006ed4 hi=0x0
virtio: interface selected: legacy
virtio: features accepted by device
virtio: queue max size 1024, using 128
virtio: desc table at 0x80001000 (16-byte aligned: yes), avail at 0x80001800, used at 0x80002000 (4096-byte aligned: yes)
virtio: device status now 0xf (DRIVER_OK set)
virtio-blk: device capacity 32768 sectors (16 MiB)

virtio-blk: write sector 0 -> rc=0 (device status OK), pattern checksum 0xf9e05dc5
virtio-blk: read  sector 0 -> rc=0 (device status OK)
virtio-blk: read checksum 0xf9e05dc5, byte mismatches 0

RESULT: PASS (512/512 bytes round-tripped through sector 0)
virtio-blk: halting.
qemu-system-riscv64: terminating on signal 15 from pid 6508 (timeout)
```

(The trailing `terminating on signal 15` line is the `timeout` wrapper
killing QEMU after the demo halted in its `wfi` loop, same as the other
modules.)

## What the numbers mean, and how each was verified

- Device discovery: the scan read the magic value at each of the 32
  mmio slots; slots 0-6 presented the magic with device id 0 (no usable
  device), slot 7 presented device id 2 (block) at `0x10008000`. The
  driver bound to slot 7.
- Interface negotiation: the device reported Version 1 and feature
  words `lo=0x31006ed4 hi=0x0`, i.e. `VIRTIO_F_VERSION_1` (bit 32) not
  offered, so the driver used the legacy queue registers. Both register
  paths are implemented; the 1.x path was additionally exercised by
  re-running the same binary with
  `-global virtio-mmio.force-legacy=false`, where the device reported
  Version 2 and features `hi=0x101`, the driver selected the 1.x
  interface, and the run also printed `RESULT: PASS` with the same
  checksums.
- Queue geometry: the device reported QueueNumMax 1024; the driver
  programmed 128 descriptors. The descriptor table landed at
  `0x80001000` (16-byte aligned, checked by masking), the used ring at
  `0x80002000` (4096-byte aligned, checked by masking).
- Device status after init read back `0xf`
  (ACKNOWLEDGE|DRIVER|FEATURES_OK|DRIVER_OK), confirming each handshake
  step.
- Capacity: config space offset 0 read as 32768 sectors, matching the
  16 MiB backing image (`32768 * 512 = 16777216`).
- Data integrity: the write returned device status OK, the read
  returned device status OK, and all 512 bytes of the read-back buffer
  equaled the written pattern (0 mismatches). The FNV-1a checksums of
  the written (`0xf9e05dc5`) and read (`0xf9e05dc5`) buffers agree.
- Host-side cross-check: after the run, sector 0 of `disk.img` was
  compared byte-for-byte against the pattern computed independently on
  the host and matched; the remaining `16 MiB - 512` bytes of the image
  were still zero, confirming the device wrote exactly the sector the
  driver requested and nothing else.

## Bring-up note (bug found and fixed during development)

The first working-queue attempt timed out in `virtio_kick_wait` with
device status `0xf`, interrupt status `0x0`, and the used ring
untouched: the device was healthy but had never been given a valid
queue. The cause was a missing `GuestPageSize` (`0x028`) write in the
legacy path: the device interprets `QueuePFN` in units of the guest
page size, and without the write it decoded the queue address as
`0x80001` instead of `0x80001000`, so it observed an empty queue at a
bogus address. Writing `GuestPageSize = 4096` before `QueuePFN` fixed
it; the passing run above is the evidence. (The legacy `QueueAlign`
register is at `0x03c`, which was also corrected from an initial wrong
offset during the same session.)
