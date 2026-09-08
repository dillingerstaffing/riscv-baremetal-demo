// virtio.c: virtio-mmio transport driver. Scans the QEMU virt board's
// virtio-mmio slots, runs the spec's device initialization sequence
// (reset, ACKNOWLEDGE, DRIVER, feature negotiation, FEATURES_OK,
// queue setup, DRIVER_OK), and manages one split virtqueue whose
// descriptor table, available ring, and used ring live in ordinary RAM.
// Feature negotiation picks the interface: if the device offers
// VIRTIO_F_VERSION_1 the 1.x queue registers (QueueDesc/Driver/Device,
// QueueReady) are used; otherwise the driver falls back to the legacy
// interface (QueueNum/QueueAlign/QueuePFN). The driver polls the used
// ring; no interrupts are used. There is no MMU in play (-bios none,
// M-mode), so guest physical addresses equal the C pointers used here.

#include "virtio.h"
#include "../uart.h"

#define MMIO_BASE   0x10001000UL
#define MMIO_STRIDE 0x1000UL
#define MMIO_SLOTS  32

// Legacy-only queue registers (absent from the 1.x interface).
// GuestPageSize tells the device the page-size unit of QueuePFN.
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_MMIO_QUEUE_PFN       0x040
#define VIRTIO_MMIO_QUEUE_ALIGN     0x03c

#define VQ_NUM 128     // descriptors we are prepared to host
#define VQ_ALIGN 4096  // alignment used for the used ring

static uint64_t vdev_base;  // MMIO base of the block device
static uint32_t vq_size;    // negotiated queue size

struct vq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_NUM];
    uint16_t used_event;
};

struct vq_used {
    uint16_t flags;
    uint16_t idx;
    struct vq_used_elem ring[VQ_NUM];
    uint16_t avail_event;
};

// One contiguous queue region: descriptor table at +0, available ring
// right after it, used ring on a VQ_ALIGN boundary. This satisfies the
// alignment rules of both the legacy layout (used ring on QueueAlign)
// and the 1.x layout (desc 16, avail 2, used 4).
#define QMEM_SIZE (VQ_ALIGN + 8 * VQ_NUM + 8)
static uint8_t qmem[QMEM_SIZE] __attribute__((aligned(VQ_ALIGN)));

static struct vq_desc *qdesc;
static struct vq_avail *qavail;
static struct vq_used *qused;

static uint16_t used_seen;  // used-ring entries consumed so far

static uint32_t reg_read(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}

static void reg_write(uint64_t base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)(base + off) = v;
}

static void mem_fence(void) {
    __asm__ volatile("fence" ::: "memory");
}

// Scan the virtio-mmio slots for a block device. Returns its MMIO base,
// or 0 if none is found.
static uint64_t find_block_device(void) {
    for (int i = 0; i < MMIO_SLOTS; i++) {
        uint64_t base = MMIO_BASE + (uint64_t)i * MMIO_STRIDE;
        if (reg_read(base, VIRTIO_MMIO_MAGIC) != VIRTIO_MAGIC_VALUE)
            continue;
        uint32_t devid = reg_read(base, VIRTIO_MMIO_DEVICE_ID);
        uint32_t ver = reg_read(base, VIRTIO_MMIO_VERSION);
        uart_puts("virtio: mmio slot ");
        uart_put_dec((unsigned long)i);
        uart_puts(" (");
        uart_put_hex(base);
        uart_puts("): device id ");
        uart_put_dec(devid);
        uart_puts(", version ");
        uart_put_dec(ver);
        uart_puts("\n");
        if (devid == VIRTIO_ID_BLOCK)
            return base;
    }
    return 0;
}

static void fail(uint64_t base, const char *msg) {
    uart_puts("virtio: ");
    uart_puts(msg);
    uart_puts("\n");
    reg_write(base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
}

uint32_t virtio_init(void) {
    uint64_t base = find_block_device();
    if (base == 0) {
        uart_puts("virtio: no block device on the mmio bus\n");
        return 0;
    }
    vdev_base = base;

    // Device status sequence from the spec.
    reg_write(base, VIRTIO_MMIO_STATUS, 0);  // reset the device
    reg_write(base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    reg_write(base, VIRTIO_MMIO_STATUS,
              VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    // Feature negotiation: read the 64 device feature bits in two halves.
    reg_write(base, VIRTIO_MMIO_DEVICE_FEAT_S, 0);
    uint32_t feat_lo = reg_read(base, VIRTIO_MMIO_DEVICE_FEAT);
    reg_write(base, VIRTIO_MMIO_DEVICE_FEAT_S, 1);
    uint32_t feat_hi = reg_read(base, VIRTIO_MMIO_DEVICE_FEAT);
    uart_puts("virtio: device features lo=");
    uart_put_hex(feat_lo);
    uart_puts(" hi=");
    uart_put_hex(feat_hi);
    uart_puts("\n");

    // Negotiate the interface: take VIRTIO_F_VERSION_1 (the 1.x
    // interface) if offered, otherwise use the legacy interface.
    int modern = (feat_hi & (1u << (VIRTIO_F_VERSION_1 - 32))) != 0;
    uart_puts("virtio: interface selected: ");
    uart_puts(modern ? "1.x (VIRTIO_F_VERSION_1)" : "legacy");
    uart_puts("\n");
    uint32_t drv_hi = modern ? (1u << (VIRTIO_F_VERSION_1 - 32)) : 0;
    reg_write(base, VIRTIO_MMIO_DRIVER_FEAT_S, 0);
    reg_write(base, VIRTIO_MMIO_DRIVER_FEAT, 0);
    reg_write(base, VIRTIO_MMIO_DRIVER_FEAT_S, 1);
    reg_write(base, VIRTIO_MMIO_DRIVER_FEAT, drv_hi);

    uint32_t status =
        VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEAT_OK;
    reg_write(base, VIRTIO_MMIO_STATUS, status);
    if ((reg_read(base, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEAT_OK) == 0) {
        fail(base, "device rejected the negotiated features");
        return 0;
    }
    uart_puts("virtio: features accepted by device\n");

    // Queue 0 setup.
    reg_write(base, VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = reg_read(base, VIRTIO_MMIO_QUEUE_MAX);
    if (qmax == 0) {
        fail(base, "queue 0 reports size 0");
        return 0;
    }
    vq_size = qmax < VQ_NUM ? qmax : VQ_NUM;

    qdesc = (struct vq_desc *)qmem;
    qavail = (struct vq_avail *)(qmem + 16 * vq_size);
    uint64_t used_off =
        ((uint64_t)16 * vq_size + 6 + 2 * vq_size + VQ_ALIGN - 1) &
        ~((uint64_t)VQ_ALIGN - 1);
    qused = (struct vq_used *)(qmem + used_off);

    uart_puts("virtio: queue max size ");
    uart_put_dec(qmax);
    uart_puts(", using ");
    uart_put_dec(vq_size);
    uart_puts("\n");
    uart_puts("virtio: desc table at ");
    uart_put_hex((unsigned long)(uintptr_t)qdesc);
    uart_puts(" (16-byte aligned: ");
    uart_puts((((uintptr_t)qdesc & 15) == 0) ? "yes" : "NO");
    uart_puts("), avail at ");
    uart_put_hex((unsigned long)(uintptr_t)qavail);
    uart_puts(", used at ");
    uart_put_hex((unsigned long)(uintptr_t)qused);
    uart_puts(" (");
    uart_put_dec(VQ_ALIGN);
    uart_puts("-byte aligned: ");
    uart_puts((((uintptr_t)qused & (VQ_ALIGN - 1)) == 0) ? "yes" : "NO");
    uart_puts(")\n");

    reg_write(base, VIRTIO_MMIO_QUEUE_NUM, vq_size);
    if (modern) {
        // 1.x interface: hand the device the three physical addresses.
        uint64_t d = (uint64_t)(uintptr_t)qdesc;
        uint64_t a = (uint64_t)(uintptr_t)qavail;
        uint64_t u = (uint64_t)(uintptr_t)qused;
        reg_write(base, VIRTIO_MMIO_QDESC_LO, (uint32_t)d);
        reg_write(base, VIRTIO_MMIO_QDESC_HI, (uint32_t)(d >> 32));
        reg_write(base, VIRTIO_MMIO_QDRV_LO, (uint32_t)a);
        reg_write(base, VIRTIO_MMIO_QDRV_HI, (uint32_t)(a >> 32));
        reg_write(base, VIRTIO_MMIO_QDEV_LO, (uint32_t)u);
        reg_write(base, VIRTIO_MMIO_QDEV_HI, (uint32_t)(u >> 32));
        reg_write(base, VIRTIO_MMIO_QUEUE_READY, 1);
    } else {
        // Legacy interface: one page-aligned region, described by its
        // page frame number and the used-ring alignment. GuestPageSize
        // must be programmed first: the device interprets QueuePFN in
        // units of that page size.
        reg_write(base, VIRTIO_MMIO_GUEST_PAGE_SIZE, VQ_ALIGN);
        reg_write(base, VIRTIO_MMIO_QUEUE_ALIGN, VQ_ALIGN);
        reg_write(base, VIRTIO_MMIO_QUEUE_PFN,
                  (uint32_t)((uint64_t)(uintptr_t)qmem >> 12));
    }

    reg_write(base, VIRTIO_MMIO_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
    uart_puts("virtio: device status now ");
    uart_put_hex(reg_read(base, VIRTIO_MMIO_STATUS));
    uart_puts(" (DRIVER_OK set)\n");
    return vq_size;
}

void virtio_desc_set(int i, uint64_t addr, uint32_t len, uint16_t flags,
                     uint16_t next) {
    qdesc[i].addr = addr;
    qdesc[i].len = len;
    qdesc[i].flags = flags;
    qdesc[i].next = next;
}

int virtio_kick_wait(uint16_t head) {
    uint16_t idx = qavail->idx;
    qavail->ring[idx % vq_size] = head;
    mem_fence();  // descriptors and ring entry visible before idx moves
    qavail->idx = idx + 1;
    mem_fence();  // idx visible before the notify write
    reg_write(vdev_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    unsigned long timeout = 100000000UL;
    while (qused->idx == used_seen) {
        mem_fence();  // force the compiler to re-read qused->idx
        if (--timeout == 0) {
            // Diagnostic dump: what did the device actually do?
            uart_puts("virtio: kick timed out; device status ");
            uart_put_hex(reg_read(vdev_base, VIRTIO_MMIO_STATUS));
            uart_puts(", intr status ");
            uart_put_hex(reg_read(vdev_base, VIRTIO_MMIO_INT_STATUS));
            uart_puts(", used idx ");
            uart_put_dec(qused->idx);
            uart_puts(", avail idx ");
            uart_put_dec(qavail->idx);
            uart_puts("\n");
            return -1;  // device never completed the request
        }
    }
    mem_fence();
    struct vq_used_elem e = qused->ring[used_seen % vq_size];
    used_seen++;
    if (e.id != head)
        return -2;  // used entry does not match the request we sent
    return (int)e.len;
}

uint64_t virtio_config64(uint32_t off) {
    uint64_t base = vdev_base + VIRTIO_MMIO_CONFIG + off;
    uint32_t lo = *(volatile uint32_t *)(uintptr_t)base;
    uint32_t hi = *(volatile uint32_t *)(uintptr_t)(base + 4);
    return ((uint64_t)hi << 32) | lo;
}

uint64_t virtio_mmio_base(void) {
    return vdev_base;
}
