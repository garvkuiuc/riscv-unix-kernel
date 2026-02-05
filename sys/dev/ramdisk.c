/*! @file ramdisk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‌‌​‌‌‌‌​⁠⁠‌
    @brief Memory-backed storage implementation
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef RAMDISK_DEBUG
#define DEBUG
#endif

#ifdef RAMDISK_TRACE
#define TRACE
#endif

#include <stddef.h>

#include "console.h"
#include "devimpl.h"
#include "error.h"
#include "heap.h"
#include "misc.h"
#include "string.h"
#include "uio.h"
#include "cache.h"

#ifndef RAMDISK_NAME
#define RAMDISK_NAME "ramdisk"
#endif

// INTERNAL TYPE DEFINITIONS
//

/**
 * @brief Storage device backed by a block of memory. Allows modification of the backing memory
 * block.
 */
struct ramdisk
{
    struct storage storage; ///< Storage struct of memory storage
    void *buf;              ///< Block of memory
    size_t size;            ///< Size of memory block
};

// INTERNAL FUNCTION DECLARATIONS
//

static int ramdisk_open(struct storage *sto);
static void ramdisk_close(struct storage *sto);
static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf,
                          unsigned long bytecnt);
static int ramdisk_cntl(struct storage *sto, int cmd, void *arg);

// INTERNAL GLOBAL CONSTANTS
//

static const struct storage_intf ramdisk_intf = {
    .blksz = CACHE_BLKSZ, // change to 1 for testing
    .open = &ramdisk_open,
    .close = &ramdisk_close,
    .fetch = &ramdisk_fetch,
    .store = NULL, // Read-only storage (blob data in .rodata)
    .cntl = &ramdisk_cntl};

// EXPORTED FUNCTION DEFINITIONS
//
/* TESTING FUNCTION------------------------------------------------
 * @brief Creates and registers a RAM disk from a caller-supplied buffer.
 * @param buf Pointer to RAM buffer to expose as block device.
 * @param len Number of bytes in the buffer.
 */
/*
void ramdisk_attach_buffer(void *buf, size_t len)
{
    if (buf == NULL || len == 0)
        panic("ramdisk_attach_buffer: invalid buffer/size");

    struct ramdisk *rd = kcalloc(1, sizeof(*rd));
    if (rd == NULL)
        panic("ramdisk_attach_buffer: out of memory");

    rd->buf = buf;
    rd->size = len;

    storage_init(&rd->storage, &ramdisk_intf, len);

    int regno = register_device(RAMDISK_NAME, DEV_STORAGE, &rd->storage);
    if (regno < 0)
        panic("ramdisk_attach_buffer: failed to register device");

    kprintf("ramdisk: attached %s%d (%lu bytes)\n",
            RAMDISK_NAME, regno, (unsigned long)len);
    
} */ // ---------------------------------------------------------------------
/**
 * @brief Creates and registers a memory-backed storage device
 * @return None
 */
void ramdisk_attach()
{
    // External symbols from linker script for embedded blob data
    extern char _kimg_blob_start[], _kimg_blob_end[];

    // allocate memory storage and create device *rd
    size_t ramdisk_size = (size_t)(_kimg_blob_end - _kimg_blob_start);

    struct ramdisk *rd = kcalloc(1, sizeof(*rd));

    if (rd == NULL)
    {
        panic("ramdisk_attach: out of memory");
    }

    rd->buf = _kimg_blob_start;
    rd->size = ramdisk_size;

    storage_init(&rd->storage, &ramdisk_intf, ramdisk_size);

    int regno = register_device(RAMDISK_NAME, DEV_STORAGE, &rd->storage);
    if (regno < 0)
    {
        panic("ramdisk_attach: failed to register device");
    }

    // info("ramdisk: attached %s%d (%zu bytes)", RAMDISK_NAME, regno, ramdisk_size);
    kprintf("ramdisk: attached %s%d (%lu bytes)\n",
            RAMDISK_NAME, regno, (unsigned long)ramdisk_size);
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * @brief Opens the _ramdisk_ device.
 * @param sto Storage struct pointer for memory storage
 * @return 0 on success
 */
static int ramdisk_open(struct storage *sto)
{
    // FIXME

    if (sto == NULL)
    {
        return -EINVAL;
    }

    //struct ramdisk *rd = (struct ramdisk *)sto;
    struct ramdisk *rd = (struct ramdisk *)((char *)sto - offsetof(struct ramdisk, storage));

    if (rd == NULL || rd->buf == NULL || rd->size == 0)
        return -EINVAL; // invalid device state

    return 0; // success
}

/**
 * @brief Closes the _ramdisk_ device.
 * @param sto Storage struct pointer for memory storage
 */
static void ramdisk_close(struct storage *sto)
{
    // FIXME
    if (sto == NULL)
    {
        return;
    }

    //struct ramdisk *rd = (struct ramdisk *)sto;
    struct ramdisk *rd = (struct ramdisk *)((char *)sto - offsetof(struct ramdisk, storage));

    // If internal pointers are invalid, stop here
    if (rd->buf == NULL || rd->size == 0)
    {
        return;
    }

    return;
}

/**
 * @brief Reads bytecnt number of bytes from the disk and writes them to buf.
 * @details Performs proper bounds checks, then copies data from memory block to passed buffer
 * @param sto Storage struct pointer for memory storage
 * @param pos Position in storage to read from
 * @param buf Buffer to copy data from memory to
 * @param bytecnt Number of bytes to read from memory
 * @return Number of bytes successfully read
 */
static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf,
                          unsigned long bytecnt)
{
    
    //struct ramdisk *rd = (struct ramdisk *)sto; // storage is first field -> OK
    struct ramdisk *rd = (struct ramdisk *)((char *)sto - offsetof(struct ramdisk, storage));

    // TEMP TRACE:
    kprintf("[ramdisk_fetch] pos=%llu len=%lu size=%lu\n",
            pos, bytecnt, (unsigned long)rd->size);

    if (!buf)
        return -EINVAL;
    if (pos >= rd->size)
        return 0;
    size_t avail = rd->size - pos;
    size_t count = (bytecnt < avail) ? bytecnt : avail;
    memcpy(buf, (const char *)rd->buf + pos, count);
    return count;
}

/**
 * @brief _cntl_ functions for memory storage.
 * @details Memory storage supports basic control operations
 * @details Any commands such as FCNTL_GETEND should pass back through the arg variable. Do not
 * directly return the value.
 * @details FCNTL_GETEND should return the capacity of the VirtIO block device in bytes.
 * @param sto Storage struct pointer for memory storage
 * @param cmd command to execute. ramdisk should support FCNTL_GETEND.
 * @param arg Argument for commands
 * @return 0 on success, error on failure or unsupported command
 */
static int ramdisk_cntl(struct storage *sto, int cmd, void *arg)
{
    // FIXME
    //struct ramdisk *rd = (struct ramdisk *)sto;
    struct ramdisk *rd = (struct ramdisk *)((char *)sto - offsetof(struct ramdisk, storage));

    if (rd == NULL)
        return -EINVAL;

    if (cmd == FCNTL_GETEND)
    {
        if (arg == NULL)
            return -EINVAL;

        *(size_t *)arg = rd->size;
        return 0;
    }

    // Any unsupported command:
    return -ENOTSUP;
}