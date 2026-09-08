// virtio.h: virtio-mmio transport register map and the split-virtqueue
// ring layouts. Offsets, status bits, and feature bits follow the
// "MMIO Device Register Layout", "Device Status Field", and "Basic
// Facilities of a Virtio Device" sections of the virtio specification.
#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>

// virtio-mmio register offsets.
#define VIRTIO_MMIO_MAGIC         0x000
#define VIRTIO_MMIO_VERSION       0x004
#define VIRTIO_MMIO_DEVICE_ID     0x008
#define VIRTIO_MMIO_VENDOR_ID     0x00c
#define VIRTIO_MMIO_DEVICE_FEAT   0x010
#define VIRTIO_MMIO_DEVICE_FEAT_S 0x014
#define VIRTIO_MMIO_DRIVER_FEAT   0x020
#define VIRTIO_MMIO_DRIVER_FEAT_S 0x024
#define VIRTIO_MMIO_QUEUE_SEL     0x030
#define VIRTIO_MMIO_QUEUE_MAX     0x034
#define VIRTIO_MMIO_QUEUE_NUM     0x038
#define VIRTIO_MMIO_QUEUE_READY   0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY  0x050
#define VIRTIO_MMIO_INT_STATUS    0x060
#define VIRTIO_MMIO_STATUS        0x070
#define VIRTIO_MMIO_QDESC_LO      0x080
#define VIRTIO_MMIO_QDESC_HI      0x084
#define VIRTIO_MMIO_QDRV_LO       0x090
#define VIRTIO_MMIO_QDRV_HI       0x094
#define VIRTIO_MMIO_QDEV_LO       0x0a0
#define VIRTIO_MMIO_QDEV_HI       0x0a4
#define VIRTIO_MMIO_CONFIG        0x100

#define VIRTIO_MAGIC_VALUE 0x74726976u  // 'v','i','r','t' as little-endian u32

// Device type we drive.
#define VIRTIO_ID_BLOCK 2

// Device status bits.
#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEAT_OK   8
#define VIRTIO_STATUS_FAILED    128

// Feature bit 32: VIRTIO_F_VERSION_1 selects the 1.x (non-legacy) device.
#define VIRTIO_F_VERSION_1 32

// Descriptor flags.
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

// One descriptor: 16 bytes, 16-byte aligned (spec: descriptor table).
struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
_Static_assert(sizeof(struct vq_desc) == 16, "vq_desc must be 16 bytes");

// One used-ring element.
struct vq_used_elem {
    uint32_t id;
    uint32_t len;
};

// Full device bring-up: discover a virtio-mmio block device, run the
// spec's status sequence, negotiate features, and set up queue 0.
// Returns the queue size in use, or 0 on failure.
uint32_t virtio_init(void);

// Fill one descriptor-table entry.
void virtio_desc_set(int i, uint64_t addr, uint32_t len, uint16_t flags,
                     uint16_t next);

// Append descriptor `head` to the available ring, notify the device, and
// poll the used ring until the request completes. Returns the used-ring
// `len` (>= 0), or a negative error on timeout / id mismatch.
int virtio_kick_wait(uint16_t head);

// Read 8 bytes of device configuration space (block device: sector count
// lives at offset 0).
uint64_t virtio_config64(uint32_t off);

// MMIO base of the discovered block device (for reporting).
uint64_t virtio_mmio_base(void);

#endif  // VIRTIO_H
