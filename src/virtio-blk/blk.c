// blk.c: block read/write on top of the virtio transport. Each request is
// a chain of three descriptors, per the spec's block-device request
// layout: a 16-byte request header (type, then a 32-bit field that is
// `reserved` on the 1.x interface and `ioprio` on the legacy interface,
// then the sector number), the 512-byte data buffer, and a one-byte
// status written back by the device. The middle header field is zero in
// both cases here. VIRTIO_BLK_T_OUT (1) writes data to the device;
// VIRTIO_BLK_T_IN (0) reads data from the device. VIRTIO_BLK_S_OK (0)
// means success.

#include "virtio.h"
#include "blk.h"

#define BLK_T_IN  0
#define BLK_T_OUT 1
#define BLK_S_OK  0

struct blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};
_Static_assert(sizeof(struct blk_req) == 16, "blk_req must be 16 bytes");

static struct blk_req req;
static uint8_t status_byte;

static int blk_op(uint64_t sector, uint8_t *data, int write) {
    req.type = write ? BLK_T_OUT : BLK_T_IN;
    req.reserved = 0;
    req.sector = sector;
    status_byte = 0xff;  // poison: the device must overwrite this

    // Descriptor 0: request header, device reads it.
    virtio_desc_set(0, (uint64_t)(uintptr_t)&req, sizeof(req),
                    VIRTQ_DESC_F_NEXT, 1);
    // Descriptor 1: data. Written by the driver for OUT, by the device
    // for IN.
    virtio_desc_set(1, (uint64_t)(uintptr_t)data, BLK_SECTOR_SIZE,
                    VIRTQ_DESC_F_NEXT | (write ? 0 : VIRTQ_DESC_F_WRITE), 2);
    // Descriptor 2: status byte, device writes it.
    virtio_desc_set(2, (uint64_t)(uintptr_t)&status_byte, 1,
                    VIRTQ_DESC_F_WRITE, 0);

    int r = virtio_kick_wait(0);
    if (r < 0)
        return r;  // transport error: timeout or id mismatch
    if (status_byte != BLK_S_OK)
        return 100 + status_byte;  // device reported failure
    return 0;
}

int blk_write(uint64_t sector, const uint8_t *data) {
    return blk_op(sector, (uint8_t *)data, 1);
}

int blk_read(uint64_t sector, uint8_t *data) {
    return blk_op(sector, data, 0);
}
