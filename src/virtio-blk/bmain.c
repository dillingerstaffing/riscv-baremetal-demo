// bmain.c: bring-up and verification for the virtio-blk module. Writes a
// deterministic 512-byte pattern to sector 0, reads it back, and compares
// every byte. The checksums and mismatch count printed below are measured
// on the QEMU run; see PROOF.md for the captured output.

#include "../uart.h"
#include "virtio.h"
#include "blk.h"

static uint8_t wbuf[BLK_SECTOR_SIZE] __attribute__((aligned(16)));
static uint8_t rbuf[BLK_SECTOR_SIZE] __attribute__((aligned(16)));

// FNV-1a 32-bit: a compact fingerprint of a buffer, so the write and
// read paths can be compared as single numbers in the log.
static uint32_t fnv1a(const uint8_t *p, unsigned long n) {
    uint32_t h = 2166136261u;
    for (unsigned long i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

int main(void) {
    uart_init();
    uart_puts("========================================\n");
    uart_puts("virtio-blk demo: block driver, sector write/read\n");
    uart_puts("========================================\n\n");

    if (virtio_init() == 0) {
        uart_puts("virtio-blk: init failed, halting.\n");
        return 1;
    }

    // Block-device config space starts with the 64-bit sector count.
    uint64_t capacity = virtio_config64(0);
    uart_puts("virtio-blk: device capacity ");
    uart_put_dec((unsigned long)capacity);
    uart_puts(" sectors (");
    uart_put_dec((unsigned long)(capacity / 2048));
    uart_puts(" MiB)\n\n");

    // Deterministic pattern: every byte is a function of its index.
    for (unsigned long i = 0; i < BLK_SECTOR_SIZE; i++)
        wbuf[i] = (uint8_t)(i ^ (i >> 8) ^ 0x5a);
    uint32_t wsum = fnv1a(wbuf, BLK_SECTOR_SIZE);

    int rc = blk_write(0, wbuf);
    uart_puts("virtio-blk: write sector 0 -> rc=");
    uart_put_dec((unsigned long)(rc < 0 ? -rc : rc));
    uart_puts(rc < 0 ? " (transport error)" : " (device status OK)");
    uart_puts(", pattern checksum 0x");
    uart_put_hex(wsum);
    uart_puts("\n");

    // Poison the read buffer so a failed read cannot look like success.
    for (unsigned long i = 0; i < BLK_SECTOR_SIZE; i++)
        rbuf[i] = 0xaa;

    rc = blk_read(0, rbuf);
    uart_puts("virtio-blk: read  sector 0 -> rc=");
    uart_put_dec((unsigned long)(rc < 0 ? -rc : rc));
    uart_puts(rc < 0 ? " (transport error)" : " (device status OK)");
    uart_puts("\n");

    uint32_t rsum = fnv1a(rbuf, BLK_SECTOR_SIZE);
    unsigned long mismatches = 0;
    unsigned long first_bad = 0;
    for (unsigned long i = 0; i < BLK_SECTOR_SIZE; i++) {
        if (rbuf[i] != wbuf[i]) {
            if (mismatches == 0)
                first_bad = i;
            mismatches++;
        }
    }

    uart_puts("virtio-blk: read checksum 0x");
    uart_put_hex(rsum);
    uart_puts(", byte mismatches ");
    uart_put_dec(mismatches);
    if (mismatches > 0) {
        uart_puts(" (first at offset ");
        uart_put_dec(first_bad);
        uart_puts(")");
    }
    uart_puts("\n\n");

    if (mismatches == 0 && rsum == wsum) {
        uart_puts("RESULT: PASS (512/512 bytes round-tripped through sector 0)\n");
    } else {
        uart_puts("RESULT: FAIL\n");
        return 1;
    }

    uart_puts("virtio-blk: halting.\n");
    return 0;
}
