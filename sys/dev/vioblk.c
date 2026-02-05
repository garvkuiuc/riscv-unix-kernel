/*! @file vioblk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‌‌​‌‌‌‌​⁠⁠‌
    @brief VirtIO block device
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "devimpl.h"
#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include <limits.h>

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "heap.h"
#include "intr.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"  // FCNTL
#include "virtio.h"

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif

// INTERNAL CONSTANT DEFINITIONS
//

// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_GEOMETRY 4
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_F_TOPOLOGY 10
#define VIRTIO_BLK_F_CONFIG_WCE 11
#define VIRTIO_BLK_F_MQ 12
#define VIRTIO_BLK_F_DISCARD 13
#define VIRTIO_BLK_F_WRITE_ZEROES 14

// GK
// Number of descriptors per request variable
#define VIOBLK_REQ_DESCS 3

// INTERNAL FUNCTION DECLARATIONS
//

/**
 * @brief Sets the virtq avail and virtq used queues such that they are available for use. (Hint,
 * read virtio.h) Enables the interupt line for the virtio device and sets necessary flags in vioblk
 * device.
 * @param sto Storage IO struct for the storage device
 * @return Return 0 on success or negative error code if error. If the given sto is already opened,
 * then return -EBUSY.
 */
static int vioblk_storage_open(struct storage* sto);

/**
 * @brief Resets the virtq avail and virtq used queues and sets necessary flags in vioblk device. If
 * the given sto is not opened, this function does nothing.
 * @param sto Storage IO struct for the storage device
 * @return None
 */
static void vioblk_storage_close(struct storage* sto);

/**
 * @brief Reads bytecnt number of bytes from the disk and writes them to buf. Achieves this by
 * repeatedly setting the appropriate registers to request a block from the disk, waiting until the
 * data has been populated in block buffer cache, and then writes that data out to buf. Thread
 * sleeps while waiting for the disk to service the request.
 * @param sto Storage IO struct for the storage device
 * @param pos The starting position for the read within the VirtIO device
 * @param buf A pointer to the buffer to fill with the read data
 * @param bytecnt The number of bytes to read from the VirtIO device into the buffer
 * @return The number of bytes read from the device, or negative error code if error
 */
static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt);

/**
 * @brief Writes bytecnt number of bytes from the parameter buf to the disk. The size of the virtio
 * device should not change. You should only overwrite existing data. Write should also not create
 * any new files. Achieves this by filling up the block buffer cache and then setting the
 * appropriate registers to request the disk write the contents of the cache to the specified block
 * location. Thread sleeps while waiting for the disk to service the request.
 * @param sto Storage IO struct for the storage device
 * @param pos The starting position for the write within the VirtIO device
 * @param buf A pointer to the buffer with the data to write
 * @param bytecnt The number of bytes to write to the VirtIO device from the buffer
 * @return The number of bytes written to the device, or negative error code if error
 */
static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt);

/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions on the VirtIO block device.
 * @details Any commands such as FCNTL_GETEND should pass back through the arg variable. Do not
 * directly return the value.
 * @details FCNTL_GETEND should return the capacity of the VirtIO block device in bytes.
 * @param sto Storage IO struct for the storage device
 * @param op Operation to execute. vioblk should support FCNTL_GETEND.
 * @param arg Argument specific to the operation being performed
 * @return Status code on the operation performed
 */
static int vioblk_storage_cntl(struct storage* sto, int op, void* arg);

/**
 * @brief The interrupt handler for the VirtIO device. When an interrupt occurs, the system will
 * call this function.
 * @param irqno The interrupt request number for the VirtIO device
 * @param aux A generic pointer for auxiliary data.
 * @return None
 */
static void vioblk_isr(int irqno, void* aux);


// GK
// Structs, Constants, Wire-formatting, Helpers - pulling from vio documentation

// Set defautl virtqueue size, have to be power of two and capped as per 2.7
// So if queue size not defined, set it to 128
#ifndef VIOBLK_QSIZE
#define VIOBLK_QSIZE 128
#endif


// Offset arithmetic that gives you a pointer to the enclosing structure, steps:
/*
    Pretends to create a pointer of struct type "container_type" at address 0
    Then it takes the address of the specified memmber which gives the byte offset of that member within the struct
    Then, it casts the actual member pointer to a character and subtracts that offset
    This calculation moves the pointer backward to the start of the struct in memory
    It then casts this back to the pointer of type "container_type" which gives a poiner to the struct
*/ 
#ifndef container_of
#define container_of(member_ptr, container_type, container_member) \
    ((container_type *)((char *)(member_ptr) - (uintptr_t)&((container_type *)0)->container_member))
#endif

// virtio-blk layout for driver and device

/* virtio-blk I/O request, three parts in memory:

    -> header which is device readable, what operation is being performed
    -> payload which holds the data being transferred
    -> 1-byte status field that holds the outcome of the request
*/ 
// Struct for the request header, from 2.7.4 which talks about message framing
struct virtio_blk_req_hdr{

    // type tells device what operation to perform, read/write 
    uint32_t type;

    // padding space for future use in alignment
    // initially written to 0 by driver
    uint32_t reserved;

    // define sector, which is the smallest addressable unit of storage (512 bytes)
    uint64_t sector;
};

// Op codes for type field where 0 is read and 1 is write, use enum to define constants
enum{

    // Data coming in (read)
    VIRTIO_BLK_T_IN = 0,

    // Data coming out (write)
    VIRTIO_BLK_T_OUT = 1,
};

// Status codes for vio to repost result of the operation to the driver
enum{

    // Status is that the IO completed succesfuly without an error
    VIRTIO_BLK_S_OK = 0,

    // Status is that the IO failed due to a problem so its an error
    VIRTIO_BLK_S_IOERR = 1,

    // Status is that the operation is not supported by the device
    VIRTIO_BLK_S_UNSUPP = 2,
};

// Beginning of struct vioblk_storage is very similar to viorng_serial from viorng.c
// Will hold the information needed for the virtio driver to interact with the virtual disk
struct vioblk_storage{
   

    // MMIO Register Block
    volatile struct virtio_mmio_regs *regs;

    // Interrupt Request line assigned to the VirtIO Device
    int irqno;

    // Virtqueue components
    // Descriptor table, each entry describes a buffer
    struct virtq_desc * desc;

    // Avaialable, which descriptors are ready
    struct virtq_avail * avail;

    // Used, which descriptors are done
    struct virtq_used * used;

    // Number of entries in the queue
    uint16_t q_size;

    // Store the index of the used descriptors we have already seen, last processed used->idx
    uint16_t used_idx_seen;

    // Stack of free descriptors, driver will read from free_stack and use indicies and then the driver will free once done
    uint16_t * free_stack;

    // Create a stack pointer for the free indices list to be able to access the top of the stack
    int free_top;

    // Allocate soace for the 3 descriptors so that there position on the stack can be remembered once they need to be popped off
    // Do this for all three in the chain, header, data, and status
    uint16_t req_desc_header[VIOBLK_QSIZE];
    uint16_t req_desc_data[VIOBLK_QSIZE];
    uint16_t req_desc_status[VIOBLK_QSIZE];

    // Allocate large memory pools to easy allocate physical addresses for the buffers
    // Store the req_hdr for every request, pretty much the input that the device reads
    struct virtio_blk_req_hdr * header_pool;

    // Store the status byte of each requst as its the output result that the device writes
    volatile uint8_t * status_pool;

    // Condition for when device finishes a request to wake threads up
    struct condition done;

    // Thread locking to protect queue in multithreading kernels as we are implementing
    struct lock queue_lock;

    // A variable to store if someone is waiting for a condition
    int waiter;

    // Create the virtual disk that vio uses and what the OS sees when dealing with vio
    struct storage blk_device;

    // If given blk size, then use that, otherwise its 512, either way define it for this struct
    unsigned int blksz;

    // 1 if device is open, 0 if closed
    int is_open;
};

// Initialize the drivers vtable, the table of function pointers that tells the OS hwo to operate the block device
// OS will invoke the function pointers in blk_device->intf because it never directly calls the vioblk functions
// Need to define a storage_intf instance so that the vioblk storage functions can be called in that interface
// blk size is initialized to 0 but will be set in vioblk_attach most likely to 512 or whatever is reported
static const struct storage_intf vioblk_storage_intf = {

    .blksz = 0,

    .open = vioblk_storage_open,
    .close = vioblk_storage_close,
    .fetch = vioblk_storage_fetch,
    .store = vioblk_storage_store,
    .cntl = vioblk_storage_cntl
};


// Helper function for reclaiming the chain
// Properly push the completed descriptors back onto the stack so that the can be reused
//static void vioblk_reclaim_chain(struct vioblk_storage *vbd, uint16_t head_idx){

    // Start at the head of the descriptor for the completed request
    //uint16_t desc_head = head_idx;

    //for(;;){

        // Get the current descriptor in the chain
        //struct virtq_desc * desc = &vbd->desc[desc_head];

        // Remember the next field before overwriting
        //uint16_t next = desc->next;

        // REturn the descriptor index to the free stack if there is room
        // free_top pooinst to the next free slot
        //if(vbd->free_top < vbd->q_size){

           // vbd->free_stack[vbd->free_top++] = desc_head;
        //}

        // If it doesnt have the next flag set, then it was the last descriptor in the chain
        //if((desc->flags & VIRTQ_DESC_F_NEXT) == 0){

            //break;
        //}

        // Follow the chain to the next indx
        //desc_head = next;
    //}

//}

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO block device. Declared and called directly from virtio.c.
/**
 * @brief Initializes virtio block device with the necessary IO operation functions and sets the
 * required feature bits.
 * @param regs Memory mapped register of Virtio
 * @param irqno Interrupt request number of the device
 * @return None
 */
void vioblk_attach(volatile struct virtio_mmio_regs* regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct vioblk_storage* vbd;
    unsigned int blksz;
    int result;

    trace("%s(regs=%p,irqno=%d)", __func__, regs, irqno);

    assert(regs->device_id == VIRTIO_ID_BLOCK);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    __sync_synchronize();  // fence o,io

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.

    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs, enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert(((blksz - 1) & blksz) == 0);

    // FIXME

    // Features were negotiated and figured out so now to allocate space for the driver state struct
    // Allocate heap memory for the vioblk_storage struct and store the pointer into vbd
    // Heap as it will need to be accessable outside of the function
    vbd = (struct vioblk_storage *) kmalloc(sizeof(* vbd));

    // If the heap allocation fails, mark device as FAILED and return
    if(!vbd){

        regs->status |= VIRTIO_STAT_FAILED;
        return;
    }

    // Initialize the struct to 0 before filling in struct fields
    memset(vbd, 0, sizeof(* vbd));

    // Save the MMIO register pointer, the hardware intterupt number, and the negotiated block size
    vbd->regs = regs;
    vbd->irqno = irqno;
    vbd->blksz = blksz;

    // Mark the device as no open for now as it is not being used yet
    vbd->is_open = 0;

    // Set the queue size to 0 unitl read from open()
    vbd->q_size = 0;

    // Inititalize used_seen index to 0 before starting
    vbd->used_idx_seen = 0;

    // Set free_stack to NULL as will be initialized in open()
    vbd->free_stack = NULL;
    
    // Stack starts empty
    vbd->free_top = 0;

    // Initialize waiter
    vbd->waiter = 0;

    // Create a condition variable to use to put threads to sleep
    condition_init(&vbd->done, "vioblk.done");

    // Initialize a lock for the virtqueue state, so that it can work without interruption
    lock_init(&vbd->queue_lock);

    // Build the dynamic interface for the standard function pointers of this driver for the vbd
    // Allocate heap space for the struct storage_intf and store pointer into dynamic_intf
    struct storage_intf * dynamic_intf = (struct storage_intf *) kmalloc(sizeof(struct storage_intf));

    // If the heap allocation fails, mark as FAILED and return
    if(!dynamic_intf){

        regs->status |= VIRTIO_STAT_FAILED;
        return;
    }

    // Copy the function pointers from the vioblk storage interface into the dynamic one to populate it
    * dynamic_intf = vioblk_storage_intf;

    // Set the negotiated block size in the dynamic interface
    dynamic_intf->blksz = blksz;

    // The dvice stores capacity in sectors instrad of bytes so convert to bytes where one sector is 512 bytes
    // Needs to be stored as ULL because the capacity can never be negative and can be much larger than 32 bits
    unsigned long long capacity_bytes = regs->config.blk.capacity * 512ULL;

    // Initialize the struct storage vbd block device with the vtable pointer dynamic interface and the device capacity
    storage_init(&vbd->blk_device, dynamic_intf, capacity_bytes);

    // Use intr_install_isr from intr.h to register the ISR functioon for this irqno
    // Pass vbd back as the aux as a pointer so that it can access device's state
    //intr_install_isr(irqno, vioblk_isr, vbd);
    enable_intr_source(irqno, VIOBLK_INTR_PRIO, vioblk_isr, vbd);

    // Now register ther device with the device manager, if failed updated status
    if(register_device(VIOBLK_NAME, DEV_STORAGE, &vbd->blk_device) != 0){

        regs->status |= VIRTIO_STAT_FAILED;
        return;
    }

    // Set the DRIVER_OK bit in status register to signal that we are ready to drive device
    regs->status |= VIRTIO_STAT_DRIVER_OK;

    // Synchronnize MMIO status so that its globally visible
    __sync_synchronize();

}

static int vioblk_storage_open(struct storage* sto) {
    // FIXME

    // Use the container_of we defined above to get vbd from the embedded wrapper
    struct vioblk_storage * vbd = container_of(sto, struct vioblk_storage, blk_device);

    // If device already opened, return -EBUSY
    if(vbd->is_open){

        return -EBUSY;
    }

    // Select virtual queue 0 as virtio queue that all queue operations will apply to
    vbd->regs->queue_sel = 0;

    // CPU finishes writing queue_sel before anything else
    __sync_synchronize();

    // Store the hardware limit for the size of virtqueue as will be used to size queue
    uint16_t qmax = (uint16_t) vbd->regs->queue_num_max;

    // Queue fail check, if device says max queue size is 0 then theres no queue so return -ENOTSUP
    if(qmax == 0){

        return -ENOTSUP;
    }

    // Using qmax and the qlen from the given QSIZE, find what the queue size should be
    uint16_t qlen  = VIOBLK_QSIZE;

    // Compare qmax and qlen and if qlen > qmax, qlen is qmax
    if(qlen > qmax){

        qlen = qmax;
    }

    // We have qlen now so write to device so taht queue 0 will be set to that 
    vbd->regs->queue_num = qlen;

    // Synchronize so that the queue_num is finished before any MMIO changes
    __sync_synchronize();

    // Set the chosedn queue length in the driver state
    vbd->q_size = qlen;

    // Now to allocate space for the desc, avail, and used rings and descroptor tables
    // For the descriptor table, there is one virtq_desc per entry so multiple size of virtq_desc by qlen and allocate
    vbd->desc = (struct virtq_desc *) kmalloc(sizeof(struct virtq_desc) * qlen);

    // For the available ring, its given from the driver to the device queue
    vbd->avail = (struct virtq_avail *) kmalloc(VIRTQ_AVAIL_SIZE(qlen));

    // Similar for the used ring except this one is device to driver, but similar allocation to avail
    vbd->used = (struct virtq_used *) kmalloc(VIRTQ_USED_SIZE(qlen));

    // Allocation fail checks, if either of them is not "true", then set to fiaed and signal memory error
    if(!vbd->desc || !vbd->avail || !vbd->used){

        vbd->regs->status |= VIRTIO_STAT_FAILED;
        return -ENOMEM;
    }

    // Zero initialize the virtqueue buffers, basically so that they start clear
    memset((void *) vbd->desc, 0, sizeof(struct virtq_desc) * qlen);
    memset((void *) vbd->avail, 0, VIRTQ_AVAIL_SIZE(qlen));
    memset((void *) vbd->used, 0, VIRTQ_USED_SIZE(qlen));

    // inititalse the descriptor positions and keep them clear to be stored in fetch and store
    // Use xFF as the value as that is an invalid queue spot
    memset(vbd->req_desc_header, 0xFF, sizeof(vbd->req_desc_header));
    memset(vbd->req_desc_data, 0xFF, sizeof(vbd->req_desc_data));
    memset(vbd->req_desc_status, 0xFF, sizeof(vbd->req_desc_status));

    
    // Attach the virtq for device communication using function from virtio.h
    // Tells device the queue id, the number of entries based on qlen, and the buffer bases
    virtio_attach_virtq(vbd->regs, 0, qlen, (uint64_t)(uintptr_t)vbd->desc, (uint64_t)(uintptr_t)vbd->used, (uint64_t)(uintptr_t)vbd->avail);

    // After wiring the addresses, enable queue 0 so that the device can use it
    virtio_enable_virtq(vbd->regs, 0);

    // Record the curent used idx as the starting point so new completions can be seen later
    vbd->used_idx_seen = vbd->used->idx;

    // Build the descriptor free-list of indicies
    // Start by allocating a stack of free indicies for the virtqueue
    vbd->free_stack = (uint16_t *) (kmalloc)(sizeof(uint16_t) * qlen);

    // Fail check, if allocation failed, set status and return error
    if(!vbd->free_stack){

        vbd->regs->status |= VIRTIO_STAT_FAILED;
        return -ENOMEM;
    }

    // Now need to fill the stack with the free descriptor indices, simple loop
    for(uint16_t i = 0; i < qlen; i++){

        // Descriptor IDs
        vbd->free_stack[i] = i;
    }

    // Set the stack pointer to one past the last valid entry so qlen as stack goes up to qlen - 1 in the loop
    vbd->free_top = qlen;

    // Allocate header/status pools
    // Make space for qlen requeust headers, one per in-flight request where each header tells device what to r/w and where
    vbd->header_pool = (struct virtio_blk_req_hdr *) (kmalloc(sizeof(struct virtio_blk_req_hdr) * qlen));

    // Space for 1 byte statuts where device writes after a request, OK or ERR
    vbd->status_pool = (volatile uint8_t *) kmalloc(qlen);

    // Allocation fail check, if failed return Error
    if(!vbd->header_pool || !vbd->status_pool){

        vbd->regs->status |= VIRTIO_STAT_FAILED;
        return -ENOMEM;
    }

    // Zero initialize the allocated memory to make sure clear to start
    memset((void *) vbd->header_pool, 0, sizeof(struct virtio_blk_req_hdr) * qlen);
    memset((void *) vbd->status_pool, 0, qlen);

    // Enable device intterups now that the queue is live and can properly recieve and execute
    enable_intr_source(vbd->irqno, VIOBLK_INTR_PRIO, vioblk_isr, vbd);

    // Mark device as opened and return
    vbd->is_open = 1;

    // Check in the console that the device was actualy opened by printing the values of the rings and set values to the console
    kprintf("vioblk_storage_open: qlen=%u irq=%d desc=%p avail=%p used=%p\n", vbd->q_size, vbd->irqno, vbd->desc, vbd->avail, vbd->used);
    return 0;
}

static void vioblk_storage_close(struct storage* sto) {
    // FIXME

    // Use the container_of we defined above to get vbd from the embedded wrapper
    struct vioblk_storage * vbd = container_of(sto, struct vioblk_storage, blk_device);

    // Check if open, if not return
    if(!vbd->is_open){

        return;
    }

    // stop interrupts from this device as it is being closed
    disable_intr_source(vbd->irqno);

    // Select virtual queue 0 as virtio queue that all queue operations will apply to
    vbd->regs->queue_sel = 0;

    // CPU finishes writing queue_sel before anything else
    __sync_synchronize();

    // Device to reset queue 0, clears ring/state to stop it from being used
    virtio_reset_virtq(vbd->regs, 0);

    // Check for any latched device interrupts
    // Local scope so that the variable pending only exists within these braces
    {
        // Read the device's intr status reg to check if any bits are set
        uint32_t pending = vbd->regs->interrupt_status;

        // if any bits are set, write the same bits to the ack register
        if(pending){

            // Acknoledeges taht the device saw the interrupts and that the intr can be cleared
            vbd->regs->interrupt_ack = pending;
        }
    }

    // Free all the queue/shared-mem allocations, use the kfree function
    if(vbd->desc){

        kfree((void *) vbd->desc);
        vbd->desc = NULL;
    }

    if(vbd->avail){

        kfree((void *) vbd->avail);
        vbd->avail = NULL;
    }

    if(vbd->used){

        kfree((void *) vbd->used);
        vbd->used = NULL;
    }

    if(vbd->free_stack){

        kfree((void *) vbd->free_stack);
        vbd->free_stack = NULL;
    }

    if(vbd->header_pool){

        kfree((void *) vbd->header_pool);
        vbd->header_pool = NULL;
    }
    
    if(vbd->status_pool){

        kfree((void *) vbd->status_pool);
        vbd->status_pool = NULL;
    }

    // Clear the driver state elements of the queue, size, seen and stack top
    vbd->q_size = 0;
    vbd->used_idx_seen = 0;
    vbd->free_top = 0;

    // Mark the device as closed
    vbd->is_open = 0;
    
}

static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt) {
    // FIXME

    // Use the container_of we defined above to get vbd from the embedded wrapper
    struct vioblk_storage * vbd = container_of(sto, struct vioblk_storage, blk_device);

    // Check if open, if not return
    if(!vbd->is_open){

        return 0;
    }

    // Check for unaligned reads
    //if((pos % vbd->blksz) != 0){

       // return 0;
    //}

    // Store the capacity of the disk in bytes
    unsigned long long cap = sto->capacity;

    // Negotiated block size
    unsigned int blksz = vbd->blksz;

    

    // Check if the starting position is greater than or at the capacity
    if(pos >= cap){

        return 0;
    }

    // Given that we still have bytes lefts as we passed through, find max bytes still available
    unsigned long long max_avail_bytes = cap - pos;
    
    // Check that we are not reading past the end of the disk
    // have to read bytecnt bytes so if its larger than max, change
    if((unsigned long long) bytecnt > max_avail_bytes){

        bytecnt = (unsigned long) max_avail_bytes;
    }

    // Start counter for how many bytes read in loop
    unsigned long bytes_read = 0;

    // Store the offset from the unaligned start
    unsigned long long offset_block = pos % blksz;

    // Need to create a case for an unaliigned start, use a temp buffer to copy the correct startig offset into the actual buffer
    if(offset_block != 0){

        // Need to read the sector that contains the pos aligning it down
        unsigned long long sector_start = pos - offset_block;

        // How much from the sectore we actually want
        unsigned long copy_len = vbd->blksz - offset_block;

        // If tis over the bytecnt, set it to bytecnt to not overrun
        if(copy_len > bytecnt){

            copy_len = bytecnt;
        }

        // Create the temp bufffer to holf the sector
        unsigned char temp[512];

        // Read exactly 1 block into the temp buffer
        long n_bytes = vioblk_storage_fetch(sto, sector_start, temp, blksz);

        // USe the blksz to read the correct number of bytes
        if(n_bytes != (long) blksz){

            if(bytes_read > 0){

                return (long) bytes_read;
            }

            return -1;
        }

        // Copy the aligned bits to what is asctually wanted
        memcpy((char *) buf, temp + offset_block, copy_len);

        // Incremeent everything
        pos += copy_len;
        buf = (char *) buf + copy_len;
        bytecnt -= copy_len;
        bytes_read += copy_len;

        // If the whole request was finished through this finish, otherwise continue as we did before
        if(bytecnt == 0){

            return (long) bytes_read;
        }
    }

    // By MP3 Errata, round down bytecnt to a multiple of blksz
    bytecnt = (bytecnt / blksz) * blksz;

    // If it rounds down to 0, return
    if(bytecnt == 0){

        return 0;
    }


    // Loop to read through required number of bytes
    while(bytes_read < bytecnt){

        // Read through the bytes in chunks
        // First iteration will be remaining bytes to reach bytecnt
        unsigned int chunk = (unsigned int)(bytecnt - bytes_read);
        

        // Cap the chunk at blksz because it cant handle larger requests
        if(chunk > vbd->blksz){

            chunk = vbd->blksz;
        }

        // Defensive check to not build 0-len request
        if(chunk == 0){

            break;
        }

        // Current read starts at absolute_pos which is the curernt posistion plus bytes done
        // VirtIO spec uses sectors, not bytes so convert bytes into sector
        unsigned long long absolute_pos = pos + bytes_read;
        unsigned long long sector = absolute_pos / 512ULL;

        // Create a pointer that points to where the chunk should be written into the buffer
        // buf is the start of the users buf, and store bytes done after bytes after the start into the pointer
        void * chunk_buf = (char *)buf + bytes_read;

        // Build the dscriptor chain, from the specs its a header, data buffer, and the status
        // Use the allocated space from the free stack to pop three ids off the stack that can be used for the descriptor chains
        // Before starting the process, lock the queue to protect it
        lock_acquire(&vbd->queue_lock);

        // If there are not enough free indicies, then release the lock
        if(vbd->free_top < 3){

            // Release the lock
            lock_release(&vbd->queue_lock);
            break;
        }

        // Pop the descriptor IDs off the stack and start the chain
        uint16_t desc_header = vbd->free_stack[--vbd->free_top];

        uint16_t desc_data = vbd->free_stack[--vbd->free_top];

        uint16_t desc_status = vbd->free_stack[--vbd->free_top];

        // Remember the position of the descriptors for this head and store it for the ISR to use to pop off the stack
        // Map the ID into the ring index by moding the queue size
        uint16_t slot = (uint16_t) (desc_header % vbd->q_size);
        
        // Store the ID's used for the requests
        vbd->req_desc_header[slot] = desc_header;
        vbd->req_desc_data[slot] = desc_data;
        vbd->req_desc_status[slot] = desc_status;
        
        // Use the decriptor header to find the header in the array of header pool and status pool that belongs to the rqueust
        // So header and status are pointers to the header and status byte of the request
        struct virtio_blk_req_hdr * header = &vbd->header_pool[desc_header];
        volatile uint8_t * status = &vbd->status_pool[desc_header];

        // fill virtio-blk request header with struct values necessary
        // Fetching so we are currently reading
        header->type = VIRTIO_BLK_T_IN; 

        // Leave this for padding
        header->reserved = 0;

        // Starting sector number on the disk where the read shoul start
        header->sector = sector;

        // Clear the statys by setting it to xffu so that the device will write the outcome
        * status = 0xFFu;

        // Fill in the descriptors for the this request and chaing them together
        // Header descriptor
        vbd->desc[desc_header].addr = (uint64_t)(uintptr_t) header;
        vbd->desc[desc_header].len = (uint32_t) sizeof(* header);

        // Continute chain by flagging next and setting next to the next chain
        vbd->desc[desc_header].flags = VIRTQ_DESC_F_NEXT;
        vbd->desc[desc_header].next = desc_data;

        // Data descriptor, device writes into the buffer
        vbd->desc[desc_data].addr = (uint64_t)(uintptr_t) chunk_buf;
        vbd->desc[desc_data].len = (uint32_t) chunk;

        // Continute chain by flagging next and setting next to the next chain
        // Descriptor will write into the buffer and then move to the other descriptor
        vbd->desc[desc_data].flags = (VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT);
        vbd->desc[desc_data].next = desc_status;

        // Status descriptor, device writes
        vbd->desc[desc_status].addr = (uint64_t)(uintptr_t) status;
        vbd->desc[desc_status].len = 1;

        // Continute chain by flagging next and setting next to the next chain
        // Descriptor will write, no need for next
        vbd->desc[desc_status].flags = VIRTQ_DESC_F_WRITE;
        vbd->desc[desc_status].next = 0;

        // Now that the descriptors are filled in, publish the request to the available ring
        uint16_t avail_idx = vbd->avail->idx;

        // Wrap around the ring when the end is reached
        // Next request to process starts at desc_header
        vbd->avail->ring[avail_idx % vbd->q_size] = desc_header;

        // Ensure that the writes to the ring are visibile before updating idk
        __sync_synchronize();

        // Update the index of the avaialle index
        vbd->avail->idx = (uint16_t)(avail_idx + 1);

        // Store what the used index was before we notify the device
        //uint16_t old_used_idx = vbd->used_idx_seen;

        // Notify the device queue that there is a request in the avail ring
        virtio_notify_avail(vbd->regs, 0);

        // Release the lock as there is no need for the queue to be protected as its not being updated
        lock_release(&vbd->queue_lock);

        // Wait for ISR to finish the rquest, just wait unitl used->idx changes
        // ISR may have already run so only sleep if no progress has occurred
        // int pie = disable_interrupts();

        // if(vbd->used_idx_seen == old_used_idx){

        int pie = disable_interrupts();
        
        if(*status == 0xFFu){

            condition_wait(&vbd->done);
        }
        
        else{

            restore_interrupts(pie);
        }

        while(*status == 0xFFu){

            //int pie = disable_interrupts();
            //if(vbd->used_idx_seen == old_used_idx){

            //vbd->waiter = 1;
            condition_wait(&vbd->done);
            //running_thread_yield();
           // }
            //restore_interrupts(pie);

        }   
        //vbd->waiter = 0;

        // restore_interrupts(pie);

        lock_acquire(&vbd->queue_lock);

         // Check what the newest entry is, used idx says its finished up tot that index, while seen is whats processed
        // q_size will be used for the wrapping of the ring buffer
        uint16_t used_idx = vbd->used->idx;
        uint16_t seen = vbd->used_idx_seen;
        uint16_t qsz = vbd->q_size;

        // Walk tbrough every new entry, loop over each one not processed yet
        while(seen != used_idx){

            // Used entries are in a ring buffer
            uint16_t slot = (uint16_t) (seen % qsz);

            // Each used element tells us which descriptor chain was completed
            struct virtq_used_elem * used_elem = &vbd->used->ring[slot];

            // Head descriptor index we had on the queue
            uint16_t head_id = used_elem->id;
            
            // Check which request was completed and where
            //kprintf("vioblk_isr: completed head=%u at slot=%u\n",head_id, slot);

            // Use the saved ids and find the slot that was used
            uint16_t slot_used = (uint16_t) (head_id % qsz);

            // Use the stired value to push them back on the right spot in the stack
            uint16_t header_pull = vbd->req_desc_header[slot_used];
            uint16_t data_pull = vbd->req_desc_data[slot_used];
            uint16_t status_pull = vbd->req_desc_status[slot_used];

            // Returns the 3 descriptor Ids from the stack
            // Check validity and then put back in correct spot

            if(status_pull != 0xFFFFu && vbd->free_top < vbd->q_size){

                vbd->free_stack[vbd->free_top++] = header_pull;
            }

            if(data_pull != 0xFFFFu && vbd->free_top < vbd->q_size){

                vbd->free_stack[vbd->free_top++] = data_pull;
            }

            if(header_pull != 0xFFFFu && vbd->free_top < vbd->q_size){

                vbd->free_stack[vbd->free_top++] = status_pull;
            }
            
            // Clear the remembered used slot to 0xFFFF
            vbd->req_desc_header[slot_used] = 0xFFFFu;
            vbd->req_desc_data[slot_used] = 0xFFFFu;
            vbd->req_desc_status[slot_used] = 0xFFFFu;

            // move to next used entr
            seen++;
        }

        // store updated what has been seen
        vbd->used_idx_seen = used_idx;

        lock_release(&vbd->queue_lock);


        // Release the lock as there is no need for the queue to be protected as its not being updated
        // lock_release(&vbd->queue_lock);
        // }

        //restore_interrupts(pie);

        // Now that the ISR has completed, figure out which descriptor chain completed
       // uint16_t completed_header = vbd->used->ring[old_used_idx % vbd->q_size].id;

        // Once we know the ID of the completed head, we can reclaim the chain and push them back onto the stack
        // AS they are not being used, they are free so they are now on the free stack
        
        // vbd->free_stack[vbd->free_top++] = desc_status;

        // vbd->free_stack[vbd->free_top++] = desc_data;

        // vbd->free_stack[vbd->free_top++] = desc_header;

        // Check the status, set the status pointer to an int so we can compare

        // Release the lock as there is no need for the queue to be protected as its not being updated
        // lock_release(&vbd->queue_lock); 

        uint8_t status_check = * status;

     

        // Check to see if the device properly finished the request
        if(status_check == VIRTIO_BLK_S_OK){

            // Chunk has been read
            bytes_read += chunk;
        }

        // If not, break
        else{

            break;
        }
    }

    // return the number of bytes read
    return (long) bytes_read;
}

static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt) {
    // FIXME

    // Use the container_of we defined above to get vbd from the embedded wrapper
    struct vioblk_storage * vbd = container_of(sto, struct vioblk_storage, blk_device);

    // Check if open, if not return
    if(!vbd->is_open){

        return 0;
    }

    // Check for unaligned reads
    //if((pos % vbd->blksz) != 0){

        //return 0;
   // }

    // Store the capacity of the disk in bytes
    unsigned long long cap = sto->capacity;

    // Negotiated block size
    unsigned int blksz = vbd->blksz;

    // Check if the starting position is greater than or at the capacity
    if(pos >= cap){

        return 0;
    }

    // Given that we still have bytes lefts as we passed through, find max bytes still available
    unsigned long long max_avail_bytes = cap - pos;
    
    // Check that we are not reading past the end of the disk
    // have to read bytecnt bytes so if its larger than max, change
    if((unsigned long long) bytecnt > max_avail_bytes){

        bytecnt = (unsigned long) max_avail_bytes;
    }

    // By MP3 Errata, round down bytecnt to a multiple of blksz
    bytecnt = (bytecnt / blksz) * blksz;

    // If it rounds down to 0, return
    if(bytecnt == 0){

        return 0;
    }

    // Start counter for how many bytes written in loop
    unsigned long bytes_written = 0;

    // Loop to read through required number of bytes
    while(bytes_written < bytecnt){

        // write through the bytes in chunks
        // First iteration will be remaining bytes to reach bytecnt
        unsigned int chunk = (unsigned int)(bytecnt - bytes_written);

        // Cap the chunk at blksz because it cant handle larger requests
        if(chunk > vbd->blksz){

            chunk = vbd->blksz;
        }

        // Defensive check to not build 0-len request
        if(chunk == 0){

            break;
        }

        // Current read starts at absolute_pos which is the curernt posistion plus bytes done
        // VirtIO spec uses sectors, not bytes so convert bytes into sector
        unsigned long long absolute_pos = pos + bytes_written;
        unsigned long long sector = absolute_pos / 512ULL;

        // Create a pointer that points to where the chunk should be written into the buffer
        // buf is the start of the users buf, and store bytes done after bytes after the start into the pointer
        void * chunk_buf = (char *)buf + bytes_written;

        // Build the dscriptor chain, from the specs its a header, data buffer, and the status
        // Use the allocated space from the free stack to pop three ids off the stack that can be used for the descriptor chains
        // Before starting the process, lock the queue to protect it
        lock_acquire(&vbd->queue_lock);

        // If there are not enough free indicies, then release the lock
        if(vbd->free_top < 3){

            // Release the lock
            lock_release(&vbd->queue_lock);
            break;
        }

        // Pop the descriptor IDs off the stack and start the chain
        uint16_t desc_header = vbd->free_stack[--vbd->free_top];

        uint16_t desc_data = vbd->free_stack[--vbd->free_top];

        uint16_t desc_status = vbd->free_stack[--vbd->free_top];

        // Remember the position of the descriptors for this head and store it for the ISR to use to pop off the stack
        // Map the ID into the ring index by moding the queue size
        uint16_t slot = (uint16_t) (desc_header % vbd->q_size);
        
        // Store the ID's used for the requests
        vbd->req_desc_header[slot] = desc_header;
        vbd->req_desc_data[slot] = desc_data;
        vbd->req_desc_status[slot] = desc_status;
        

        // Use the decriptor header to find the header in the array of header pool and status pool that belongs to the rqueust
        // So header and status are pointers to the header and status byte of the request
        struct virtio_blk_req_hdr * header = &vbd->header_pool[desc_header];
        volatile uint8_t * status = &vbd->status_pool[desc_header];

        // fill virtio-blk request header with struct values necessary
        // Fetching so we are currently writing
        header->type = VIRTIO_BLK_T_OUT; 

        // Leave this for padding
        header->reserved = 0;

        // Starting sector number on the disk where the read shoul start
        header->sector = sector;

        // Clear the statys by setting it to xffu so that the device will write the outcome
        * status = 0xFFu;

        // Fill in the descriptors for the this request and chaing them together
        // Header descriptor
        vbd->desc[desc_header].addr = (uint64_t)(uintptr_t) header;
        vbd->desc[desc_header].len = (uint32_t) sizeof(* header);

        // Continute chain by flagging next and setting next to the next chain
        vbd->desc[desc_header].flags = VIRTQ_DESC_F_NEXT;
        vbd->desc[desc_header].next = desc_data;

        // Data descriptor, device writes into the buffer
        vbd->desc[desc_data].addr = (uint64_t)(uintptr_t) chunk_buf;
        vbd->desc[desc_data].len = (uint32_t) chunk;

        // Continute chain by flagging next and setting next to the next chain
        // Descriptor will move into the descriptor
        vbd->desc[desc_data].flags = VIRTQ_DESC_F_NEXT;
        vbd->desc[desc_data].next = desc_status;

        // Status descriptor, device writes
        vbd->desc[desc_status].addr = (uint64_t)(uintptr_t) status;
        vbd->desc[desc_status].len = 1;

        // Continute chain by flagging next and setting next to the next chain
        // Descriptor will write, no need for next
        vbd->desc[desc_status].flags = VIRTQ_DESC_F_WRITE;
        vbd->desc[desc_status].next = 0;

        // Now that the descriptors are filled in, publish the request to the available ring
        uint16_t avail_idx = vbd->avail->idx;

        // Wrap around the ring when the end is reached
        // Next request to process starts at desc_header
        vbd->avail->ring[avail_idx % vbd->q_size] = desc_header;

        // Ensure that the writes to the ring are visibile before updating idk
        __sync_synchronize();

        // Update the index of the avaialle index
        vbd->avail->idx = (uint16_t)(avail_idx + 1);

        // Store what the used index was before we notify the device
        //uint16_t old_used_idx = vbd->used_idx_seen;

        // Notify the device queue that there is a request in the avail ring
        virtio_notify_avail(vbd->regs, 0);

        
        // Release the lock as there is no need for the queue to be protected as its not being updated
        lock_release(&vbd->queue_lock);

         // Wait for ISR to finish the rquest, just wait unitl used->idx changes
        // ISR may have already run so only sleep if no progress has occurred
        //int pie = disable_interrupts();

        //if(vbd->used_idx_seen == old_used_idx){

        int pie = disable_interrupts();

        if(*status == 0xFFu){

            condition_wait(&vbd->done);
        }

        else{

            restore_interrupts(pie);
        }

        while(*status == 0xFFu){

            //int pie = disable_interrupts();
            //if(vbd->used_idx_seen == old_used_idx){

            //vbd->waiter = 1;
            condition_wait(&vbd->done);
            //running_thread_yield();
           // }s
            //restore_interrupts(pie);

        }   
        //vbd->waiter = 0;

        //restore_interrupts(pie);

        lock_acquire(&vbd->queue_lock);

        // Check what the newest entry is, used idx says its finished up tot that index, while seen is whats processed
        // q_size will be used for the wrapping of the ring buffer
        uint16_t used_idx = vbd->used->idx;
        uint16_t seen = vbd->used_idx_seen;
        uint16_t qsz = vbd->q_size;

        // Walk tbrough every new entry, loop over each one not processed yet
        while(seen != used_idx){

            // Used entries are in a ring buffer
            uint16_t slot = (uint16_t) (seen % qsz);

            // Each used element tells us which descriptor chain was completed
            struct virtq_used_elem * used_elem = &vbd->used->ring[slot];

            // Head descriptor index we had on the queue
            uint16_t head_id = used_elem->id;
            
            // Check which request was completed and where
            kprintf("vioblk_isr: completed head=%u at slot=%u\n",head_id, slot);

            // Use the saved ids and find the slot that was used
            uint16_t slot_used = (uint16_t) (head_id % qsz);

            // Use the stired value to push them back on the right spot in the stack
            uint16_t header_pull = vbd->req_desc_header[slot_used];
            uint16_t data_pull = vbd->req_desc_data[slot_used];
            uint16_t status_pull = vbd->req_desc_status[slot_used];

            // Returns the 3 descriptor Ids from the stack
            // Check validity and then put back in correct spot

            if(status_pull != 0xFFFFu && vbd->free_top < vbd->q_size){

                vbd->free_stack[vbd->free_top++] = header_pull;
            }

            if(data_pull != 0xFFFFu && vbd->free_top < vbd->q_size){

                vbd->free_stack[vbd->free_top++] = data_pull;
            }

            if(header_pull != 0xFFFFu && vbd->free_top < vbd->q_size){

                vbd->free_stack[vbd->free_top++] = status_pull;
            }
            
            // Clear the remembered used slot to 0xFFFF
            vbd->req_desc_header[slot_used] = 0xFFFFu;
            vbd->req_desc_data[slot_used] = 0xFFFFu;
            vbd->req_desc_status[slot_used] = 0xFFFFu;

            // move to next used entr
            seen++;
        }

        // store updated what has been seen
        vbd->used_idx_seen = used_idx;

        lock_release(&vbd->queue_lock);

        // Release the lock as there is no need for the queue to be protected as its not being updated
        //lock_release(&vbd->queue_lock);

        //}
        // restore_interrupts(pie);


        // Now that the ISR has completed, figure out which descriptor chain completed
        //uint16_t completed_header = vbd->used->ring[old_used_idx % vbd->q_size].id;

        // Once we know the ID of the completed head, we can reclaim the chain and push them back onto the stack
        // AS they are not being used, they are free so they are now on the free stack
        // vbd->free_stack[vbd->free_top++] = desc_status;

        // vbd->free_stack[vbd->free_top++] = desc_data;

        // vbd->free_stack[vbd->free_top++] = desc_header;

        // Release the lock as there is no need for the queue to be protected as its not being updated
        // lock_release(&vbd->queue_lock);

        // Check the status, set the status pointer to an int so we can compare
        uint8_t status_check = * status;

       

        // Check to see if the device properly finished the request
        if(status_check == VIRTIO_BLK_S_OK){

            // Chunk has been read
            bytes_written += chunk;
        }

        // If not, break
        else{

            break;
        }
    }

    return (long) bytes_written;
}

static int vioblk_storage_cntl(struct storage* sto, int op, void* arg) {
    // FIXME

    // If the operation is GETEND, return the capacity in bytes
    if(op == FCNTL_GETEND){

        // If the caller does not provide a valid pointe, then return an error
        if(arg == NULL){

            return -EINVAL;
        }

        // Write capacity of the the storage into the argument
        * (unsigned long long *) arg = sto->capacity;

        // return 0 for success
        return 0;
    }

    // Return not supported for anything else
    return -ENOTSUP;
}

// ISR helper to check if any thread is waiting, ran into an assertion error so this to fix that
//static inline int conditon_has_waiters(struct condition * c){

   // return c->wait_list.head != NULL;
// }

static void vioblk_isr(int irqno, void* aux) {
    // FIXME

    // Aux is the pointer passed in the install isr function in the attach function
    struct vioblk_storage * vbd = (struct vioblk_storage *) aux;


    // Debug checks as for ISR, checking to make sure it initializes properly
    //kprintf("vioblk_isr: irq=%d aux=%p\n", irqno, vbd);

    // If the aux was null, dont do anything
    if(vbd == NULL){

        // Return null to console incase
        //kprintf("vioblk_isr: vbd is NULL, bailing\n");
        return;
    }

    // Check what the device is saaying using the interrupt status
    // If the bit is - then the device is saying used ring update
    uint32_t pending = vbd->regs->interrupt_status;

    // Print pending to the console to check f the device did say something
    //kprintf("vioblk_isr: interrupt_status=0x%x\n", pending);

    // In the case of 0, do nothing
    if(pending == 0){

        // Return that nothing was set incase
        //kprintf("vioblk_isr: no bits set, returning\n");
        return;
    }

    // Set the acknowoldege bits to pending
    // acknowledge the interrupt so that the device can take note of that
    vbd->regs->interrupt_ack = pending;

    // Now we need to lock the queue as we are modifying
    // Queue lock to procted the used, free stack and the used seen
   // lock_acquire(&vbd->queue_lock);

    // Check if someone is waiting, if they are, then broadcast
    

    condition_broadcast(&vbd->done);
    

    // Release the lock as we are done changing any virtqueue structure
    //lock_release(&vbd->queue_lock);

    // wake up any thread that was waiting for a request to complete
    // Use helper to check that there is a thread waiting to broadcast to
    //if(conditon_has_waiters(&vbd->done)){
    //int pie = disable_interrupts();
    //condition_broadcast(&vbd->done);
    //restore_interrupts(pie);
    //}
}
