// blk.h: block read/write on the virtio transport.
#ifndef BLK_H
#define BLK_H

#include <stdint.h>

#define BLK_SECTOR_SIZE 512

// Write one 512-byte sector. Returns 0 on success, negative on transport
// error, positive on a non-OK device status byte.
int blk_write(uint64_t sector, const uint8_t *data);

// Read one 512-byte sector. Same return convention as blk_write.
int blk_read(uint64_t sector, uint8_t *data);

#endif  // BLK_H
