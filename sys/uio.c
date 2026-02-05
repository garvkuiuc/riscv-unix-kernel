/*! @file uio.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‌‌​‌‌‌‌​⁠⁠‌
    @brief Uniform I/O interface
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef UIO_DEBUG
#define DEBUG
#endif

#ifdef UIO_TRACE
#define TRACE
#endif

#include "uio.h"

#include <stddef.h>  // for NULL and offsetof

#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "intr.h"
#include "uioimpl.h"

static void nulluio_close(struct uio* uio);

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz);

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen);

static void pipe_close_writer(struct uio *uio);
static void pipe_close_reader(struct uio *uio);
static long pipe_read_endpoint(struct uio *uio, void *buf, unsigned long bufsz);
static long pipe_write_endpoint(struct uio *uio, const void *buf, unsigned long buflen);
// static void pipe_free_backing(struct pipe_chan *chan);


struct pipe_chan{
    struct uio writer_end; //pipe write end
    struct uio reader_end; //pipe read end
    char *storage; //buffer storage
    unsigned long capacity; //buffer size
    unsigned long read_pos; //read index
    unsigned long write_pos; //write index
    unsigned long used_bytes; //bytes in buffer

    int reader_alive; //reader open flag
    int writer_alive; //writer open flag
    struct condition readable; //reader wait condition
    struct condition writable; //writer wait condition
};


static const struct uio_intf pipe_writer_vtab = {
    .close = &pipe_close_writer, //writer close
    .read  = NULL, 
    .write = &pipe_write_endpoint, //writer write op
    .cntl  = NULL 
};

static const struct uio_intf pipe_reader_vtab = {
    .close = &pipe_close_reader, //reader close
    .read  = &pipe_read_endpoint, //reader read op
    .write = NULL, 
    .cntl  = NULL 
};

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

void uio_close(struct uio* uio) {
    debug("uio_close: refcnt=%d, has_close=%d", uio->refcnt, (uio->intf->close != NULL));

    // Decrement reference count if it's greater than 0
    if (uio->refcnt > 0) {
        uio->refcnt--;
        debug("uio_close: decremented refcnt to %d", uio->refcnt);
    }

    // Only call the actual close method when refcnt reaches 0
    if (uio->refcnt == 0 && uio->intf->close != NULL) {
        debug("uio_close: calling close method");
        uio->intf->close(uio);
    } else if (uio->refcnt > 0) {
        debug("uio_close: NOT calling close (refcnt=%d still has references)", uio->refcnt);
    }
}

long uio_read(struct uio* uio, void* buf, unsigned long bufsz) {

    if (uio->intf->read != NULL) {
        if (0 <= (long)bufsz)
            return uio->intf->read(uio, buf, bufsz);
        else
            return -EINVAL;
    } else
        return -ENOTSUP;
}

long uio_write(struct uio* uio, const void* buf, unsigned long buflen) {


    if (uio->intf->write != NULL) {
        if (0 <= (long)buflen)
            return uio->intf->write(uio, buf, buflen);
        else
            return -EINVAL;
    } else
        return -ENOTSUP;
}

int uio_cntl(struct uio* uio, int op, void* arg) {
    if (uio->intf->cntl != NULL)
        return uio->intf->cntl(uio, op, arg);
    else
        return -ENOTSUP;
}

unsigned long uio_refcnt(const struct uio* uio) {
    assert(uio != NULL);
    return uio->refcnt;
}

int uio_addref(struct uio* uio) { return ++uio->refcnt; }

struct uio* create_null_uio(void) {
    static const struct uio_intf nulluio_intf = {
        .close = &nulluio_close, .read = &nulluio_read, .write = &nulluio_write};

    static struct uio nulluio = {.intf = &nulluio_intf, .refcnt = 0};

    return &nulluio;
}

static void nulluio_close(struct uio* uio) {
    // ...
}

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    // ...
    return -ENOTSUP;
}

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    // ...
    return -ENOTSUP;
}


static void pipe_free_backing(struct pipe_chan *chan){
    if(chan == NULL){
        return; //early return if null
    }

    if(chan->storage != NULL){
        kfree(chan->storage); //free buffer
        chan->storage = NULL; //clear buffer pointer
    }

    kfree(chan); //free pipe struct
}

static void pipe_close_writer(struct uio *uio){
    struct pipe_chan *chan = (struct pipe_chan *)((char *)uio - offsetof(struct pipe_chan, writer_end)); //get pipe from writer uio
    int need_free = 0; //flag: free pipe after unlock

    long flags = disable_interrupts(); //enter critical section

    if(chan->writer_alive != 0) {
        chan->writer_alive = 0; //mark writer closed
        condition_broadcast(&chan->readable); //wake any waiting readers
    }

    if(chan->reader_alive == 0){
        need_free = 1; //both ends closed, free needed
    }

    restore_interrupts(flags); //leave critical section

    if(need_free){
        pipe_free_backing(chan); //free pipe backing
    }
}

static void pipe_close_reader(struct uio *uio){ 
    struct pipe_chan *chan = (struct pipe_chan *)((char *)uio - offsetof(struct pipe_chan, reader_end)); //get pipe from reader uio
    long flags; //saved interrupt state
    int need_free = 0; //flag to free pipe

    flags = disable_interrupts(); //enter critical section

    if (chan->reader_alive != 0){ 
        chan->reader_alive = 0; //mark reader closed
        condition_broadcast(&chan->writable); //wake writers waiting for space
    }

    if(chan->writer_alive == 0){ 
        need_free = 1; //both ends closed, free needed
    }

    restore_interrupts(flags); //leave critical section

    if(need_free){
        pipe_free_backing(chan); //free pipe backing
    }
}

void create_pipe(struct uio **wptr, struct uio **rptr){
    struct pipe_chan *chan; //pipe backing object
    char *buf; //buffer for pipe data

    // GK changes
    // Create a variable that stores the capacity of the buffer
    // Doing this due to the errors when running pipes
    size_t cap;

    if(wptr != NULL){
        *wptr = NULL; //default write endpoint to NULL
    }
    if(rptr != NULL){
        *rptr = NULL; //default read endpoint to NULL
    }

    // Need to respect the max space that can be allocated
    // First set cap to the page size
    cap = PAGE_SIZE;
    
    // Now compare to the max value and if its greater, set to the max value
    if(cap > HEAP_ALLOC_MAX){

        cap = HEAP_ALLOC_MAX;
    }
    
    // Change the kmalloc(PAGE_SIZE) to kmalloc(cap)
    buf = kmalloc(cap); //allocate buffer page


    if(buf == NULL){
        return; //allocation failed
    }

    chan = kcalloc(1, sizeof(*chan)); //allocate and zero pipe struct

    if(chan == NULL){
        kfree(buf); //free buffer on failure
        return;
    }

    chan->storage= buf; //set buffer pointer
    chan->capacity= cap; //buffer size --// GK change to cap
    chan->read_pos = 0; //start read index
    chan->write_pos= 0; //start write index
    chan->used_bytes = 0; //buffer empty
    chan->reader_alive = 1; //reader open
    chan->writer_alive = 1; //writer open

    condition_init(&chan->readable, "pipe-read"); //init readable condition
    condition_init(&chan->writable, "pipe-write"); //init writable condition
    uio_init1(&chan->writer_end, &pipe_writer_vtab); //init writer uio endpoint
    uio_init1(&chan->reader_end, &pipe_reader_vtab); //init reader uio endpoint
    if(wptr != NULL){
        *wptr = &chan->writer_end; //return write end
    }
    
    if(rptr != NULL){
        *rptr = &chan->reader_end; //return read end
    }
}

static long pipe_read_endpoint(struct uio *uio, void *buf, unsigned long bufsz){
    struct pipe_chan *chan = (struct pipe_chan *)((char *)uio - offsetof(struct pipe_chan, reader_end)); //get pipe from reader uio
    unsigned long copied = 0; //bytes copied to user buffer
    unsigned long remaining; //bytes left to read
    unsigned long available; //bytes available in pipe
    unsigned long until_end; //bytes until end of buffer
    unsigned long chunk; //bytes to copy this iteration
    long flags; //saved interrupt state

    if(bufsz == 0){
        return 0; //nothing to read
    }

    flags = disable_interrupts(); //enter critical section

    while(1){
        if(chan->used_bytes == 0){
            if(chan->writer_alive == 0){
                restore_interrupts(flags); //leave critical section
                return copied; //EOF or partial read
            }
            condition_wait(&chan->readable); //wait for data
            continue; //recheck after wakeup
        }

        remaining = bufsz - copied; //space left in user buffer
        available = chan->used_bytes; //data in pipe
        until_end = chan->capacity - chan->read_pos; //space until wrap
        chunk = remaining; //start with requested amount

        if(chunk > available){
            chunk = available; //don't read more than available
        }
        if(chunk > until_end){
            chunk = until_end; //don't wrap in one memcpy
        }

        memcpy((char *)buf + copied, chan->storage + chan->read_pos, chunk); //copy from pipe to user

        chan->read_pos = (chan->read_pos + chunk) % chan->capacity; //advance read index
        chan->used_bytes -= chunk; //shrink used count
        copied += chunk; //grow copied count

        condition_broadcast(&chan->writable); //wake writers (space freed)

        if(copied != 0 || copied == bufsz){
            restore_interrupts(flags); //leave critical section
            return copied; //return bytes read
        }
    }
}



static long pipe_write_endpoint(struct uio *uio, const void *buf, unsigned long buflen){
    struct pipe_chan *chan = (struct pipe_chan *)((char *)uio - offsetof(struct pipe_chan, writer_end)); //get pipe from writer uio
    unsigned long transferred = 0; //bytes written so far
    unsigned long remaining; //bytes left to write
    unsigned long free_bytes; //free space in buffer
    unsigned long until_end; //space until end of buffer
    unsigned long chunk; //bytes to write this loop
    long flags; //saved interrupt state
    if(buflen == 0){
        return 0; //nothing to write
    }

    flags = disable_interrupts(); //enter critical section
    while(transferred < buflen){
        if(chan->reader_alive == 0){
            long ret = (transferred > 0) ? (long)transferred : -EPIPE; //partial or EPIPE
            restore_interrupts(flags); //leave critical section
            return ret; //broken pipe or partial write
        }

        if(chan->used_bytes == chan->capacity){
            condition_wait(&chan->writable); //wait for space
            continue; //recheck after wakeup
        }

        remaining = buflen - transferred; //user bytes left
        free_bytes = chan->capacity - chan->used_bytes; //free buffer space
        until_end = chan->capacity - chan->write_pos; //space until wrap
        chunk = remaining; //start with requested

        if(chunk > free_bytes){
            chunk = free_bytes; //don't exceed free space
        }

        if(chunk > until_end){
            chunk = until_end; //don't wrap in one memcpy
        }

        memcpy(chan->storage + chan->write_pos, (const char *)buf + transferred, chunk); //copy into pipe

        chan->write_pos = (chan->write_pos + chunk) % chan->capacity; //advance write index
        chan->used_bytes += chunk; //grow used count
        transferred += chunk; //grow written count

        condition_broadcast(&chan->readable); //wake readers (data available)
    }
    restore_interrupts(flags); //leave critical section
    return (long)transferred; //return bytes written
}
