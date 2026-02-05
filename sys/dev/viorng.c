// viorng.c - VirtIO rng device
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "error.h"
#include "string.h"
#include "thread.h"
#include "devimpl.h"
#include "misc.h"
#include "conf.h"
#include "intr.h"
#include "console.h"

// INTERNAL CONSTANT DEFINITIONS
//

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "viorng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

// INTERNAL TYPE DEFINITIONS
//

/* Structure: viorng_serial
   Description: Represents a VirtIO RNG (random number generator) device attached to
                the system as a serial-like interface. Holds references to MMIO registers,
                interrupt information, virtqueue descriptors, and data buffers for
                random number generation.
   Fields:
      struct serial base
          - Base serial device structure (for integration with serial interface API).
      volatile struct virtio_mmio_regs *regs
          - Pointer to VirtIO MMIO registers mapped for this device.
      int irqno
          - Interrupt request (IRQ) number assigned to this VirtIO RNG device.
      char opened
          - Flag indicating whether the device is currently opened (1) or closed (0).

      struct virtq_desc desc
          - Descriptor entry used to describe the buffer exposed to the VirtIO device.
      uint16_t table_size
          - Size of the virtqueue descriptor/avail/used rings (fixed to 1 here).

      union { struct virtq_avail avail; char buf_avail[VIRTQ_AVAIL_SIZE(1)]; }
          - Available ring structure for VirtIO queue, used by the driver to notify
            the device of available descriptors.
          - Stored in a union with raw buffer space to satisfy memory alignment
            and size requirements.

      union { volatile struct virtq_used used; char buf_used[VIRTQ_USED_SIZE(1)]; }
          - Used ring structure for VirtIO queue, written by the device to indicate
            completed descriptor operations.
          - Stored in a union with raw buffer space to satisfy memory alignment
            and size requirements.

      uint8_t *buf
          - Pointer to the buffer allocated for random data returned by the device.
      uint32_t buf_len
          - Length of the allocated buffer (in bytes).
      volatile int data_ready
          - Flag set by the device interrupt handler to indicate that new random
            data has been generated and is ready for retrieval.
*/
struct viorng_serial
{
    struct serial base;                     // Base serial device structure
    volatile struct virtio_mmio_regs *regs; // MMIO register pointer for VirtIO device
    int irqno;                              // IRQ number assigned to device
    char opened;                            // Opened flag (1 if device open, else 0)

    struct virtq_desc desc; // Virtqueue descriptor for data buffer
    uint16_t table_size;    // Size of the virtqueue table (fixed to 1)

    union
    {
        struct virtq_avail avail;            // Available ring for VirtIO queue
        char buf_avail[VIRTQ_AVAIL_SIZE(1)]; // Raw buffer for alignment/size
    };

    union
    {
        volatile struct virtq_used used;   // Used ring for VirtIO queue
        char buf_used[VIRTQ_USED_SIZE(1)]; // Raw buffer for alignment/size
    };

    uint8_t *buf;            // Pointer to buffer for random data
    uint32_t buf_len;        // Length of buffer in bytes
    volatile int data_ready; // Flag: 1 if new random data is available
    struct condition ready; // CP3: used to check conditions for spin waiting
};

// INTERNAL FUNCTION DECLARATIONS
//

static int viorng_serial_open(struct serial * ser);

static void viorng_serial_close(struct serial * ser);

static int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);

static void viorng_isr(int irqno, void * aux);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf viorng_serial_intf = {
    .blksz = 1,
    .open = &viorng_serial_open,
    .close = &viorng_serial_close,
    .recv = &viorng_serial_recv
};

// EXPORTED FUNCTION DEFINITIONS
//

/* Function Interface:
    void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno)
    Inputs: volatile struct virtio_mmio_regs * regs - pointer to VirtIO MMIO registers
            int irqno - interrupt number assigned to the VirtIO RNG device
    Outputs: None
    Description: Attaches a VirtIO RNG device by negotiating features, allocating and
                 initializing the driver structure, attaching the virtqueue, and registering
                 the device with the serial interface.
    Side Effects: - Writes to VirtIO device MMIO registers
                  - Allocates memory for driver structures and buffer
                  - Registers the device with the system device manager
*/
void viorng_attach(volatile struct virtio_mmio_regs *regs, int irqno)
{
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct viorng_serial *vrng;
    int result;

    assert(regs->device_id == VIRTIO_ID_RNG);

    // Signal device that we found a driver
    regs->status |= VIRTIO_STAT_DRIVER;
    __sync_synchronize(); // memory fence

    // Negotiate VirtIO features
    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
                                       enabled_features, wanted_features, needed_features);

    if (result != 0)
    {
        //kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // Allocate and initialize driver struct
    vrng = kcalloc(1, sizeof(struct viorng_serial));
    vrng->regs = regs;
    vrng->irqno = irqno;
    vrng->opened = 0;
    vrng->buf_len = VIORNG_BUFSZ;
    vrng->data_ready = 0;
    vrng->table_size = 1;
    vrng->buf = kcalloc(1, vrng->buf_len);

    //CP3: add condition 
    condition_init(&vrng->ready, "viorng");

    // Inform device driver is ready
    regs->status |= VIRTIO_STAT_DRIVER_OK;
    __sync_synchronize();

    // Attach the virtqueue
    virtio_attach_virtq(regs, 0, vrng->table_size,
                        (uint64_t)(uintptr_t)&vrng->desc,
                        (uint64_t)(uintptr_t)&vrng->used,
                        (uint64_t)(uintptr_t)&vrng->avail);

    // Initialize serial interface and register device
    serial_init(&vrng->base, &viorng_serial_intf);
    register_device(VIORNG_NAME, DEV_SERIAL, vrng);
}

/* Function Interface:
    int viorng_serial_open(struct serial * ser)
    Inputs: struct serial * ser - pointer to the VirtIO RNG device serial structure
    Outputs: Returns 0 on success, or -EBUSY if the device is already opened.
    Description: Opens the VirtIO RNG device by preparing the descriptor,
                 enabling the virtqueue, and registering the interrupt handler.
    Side Effects: - Configures VirtIO descriptor
                  - Enables VirtIO queue
                  - Enables interrupt source
*/
static int viorng_serial_open(struct serial *ser)
{
    struct viorng_serial *v =
        (struct viorng_serial *)((char *)ser - offsetof(struct viorng_serial, base));

    if (v->opened) 
        return -EBUSY; // check if opened

    // Configure descriptor for device to write random data
    v->desc.addr = (uint64_t)(uintptr_t)v->buf;
    v->desc.len = v->buf_len;
    v->desc.flags = VIRTQ_DESC_F_WRITE;
    v->desc.next = 0;

    // Enable the virtqueue
    virtio_enable_virtq(v->regs, 0);

    // Register interrupt handler
    enable_intr_source(v->irqno, VIORNG_IRQ_PRIO, viorng_isr, v);

    v->opened = 1;
    return 0;
}

/* Function Interface:
    void viorng_serial_close(struct serial * ser)
    Inputs: struct serial * ser - pointer to the VirtIO RNG device serial structure
    Outputs: None
    Description: Closes the VirtIO RNG device by disabling the virtqueue and
                 unregistering the interrupt handler.
    Side Effects: - Resets VirtIO virtqueue
                  - Disables interrupt source
*/
static void viorng_serial_close(struct serial *ser)
{
    struct viorng_serial *v =
        (struct viorng_serial *)((char *)ser - offsetof(struct viorng_serial, base));

    if (!v->opened)
        return;

    virtio_reset_virtq(v->regs, 0); // close serial communication
    disable_intr_source(v->irqno);
    v->opened = 0;
}

/* Function Interface:
    int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz)
    Inputs: struct serial * ser - pointer to the VirtIO RNG device serial structure
            void * buf - buffer to copy received random data into
            unsigned int bufsz - size of the buffer
    Outputs: Returns the number of bytes copied to buf,
             or -EINVAL if device is not opened.
    Description: Requests random data from the VirtIO RNG device and blocks until
                 completion, then copies the data into the provided buffer.
    Side Effects: - Notifies VirtIO device of available descriptor
                  - Spins until device signals completion
                  - Reads random data into caller buffer
*/
static int viorng_serial_recv(struct serial *ser, void *buf, unsigned int bufsz)
{
    struct viorng_serial *v =
        (struct viorng_serial *)((char *)ser - offsetof(struct viorng_serial, base));
    unsigned int i, n;

    if (!v->opened)
        return -EINVAL;
    if (bufsz == 0)
        return 0; // instantiate device

    v->data_ready = 0;

    // Make descriptor visible to device
    v->avail.idx++;
    v->avail.ring[v->avail.idx % v->table_size] = 0;
    __sync_synchronize();
    //kprintf("[viorng] Requesting data: avail.idx=%d, used.idx=%d\n", v->avail.idx, v->used.idx);
    // Notify device queue is ready
    virtio_notify_avail(v->regs, 0);
    //kprintf("[viorng-isr] Data ready: avail.idx=%d, used.idx=%d\n", v->avail.idx, v->used.idx);
    // Spin until device completes transfer

    //CP3: Now use conditions to wait
    while (v->avail.idx != v->used.idx) {
        condition_wait(&v->ready);
        //kprintf("[viorng] After wakeup: avail.idx=%d, used.idx=%d\n", v->avail.idx, v->used.idx);
    }

    // Copy back to caller buffer
    n = (bufsz < v->buf_len) ? bufsz : v->buf_len;
    for (i = 0; i < n; i++)
        ((uint8_t *)buf)[i] = v->buf[i];

    return n;
}

/* Function Interface:
    void viorng_isr(int irqno, void * aux)
    Inputs: int irqno - interrupt number
            void * aux - pointer to VirtIO RNG device structure
    Outputs: None
    Description: Interrupt service routine for VirtIO RNG. Acknowledges the interrupt
                 by writing back to the interrupt acknowledge register.
    Side Effects: - Reads and clears VirtIO interrupt status register
*/
static void viorng_isr(int irqno, void *aux)
{
    struct viorng_serial *v = (struct viorng_serial *)aux;

    if (v->regs->interrupt_status != 0) {
        v->regs->interrupt_ack = v->regs->interrupt_status;
        // CP3: Mark data ready and wake any waiting receivers
        v->data_ready = 1;
        condition_broadcast(&v->ready);
        //kprintf("[viorng-isr] Interrupt acknowledged, broadcasting\n");
    }
}