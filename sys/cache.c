/*! @file cache.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‌‌​‌‌‌‌​⁠⁠‌
    @brief Block cache for a storage device.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef CACHE_TRACE
#define TRACE
#endif

#ifdef CACHE_DEBUG
#define DEBUG
#endif

#include "cache.h"

#include "conf.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include <stdbool.h>


// INTERNAL TYPE DEFINITIONS
//

static int cache_evict_entry(struct cache *cache);


struct cache_entry{
    unsigned int block_n;// block number on disk that this entry corresponds to
    unsigned char *data;// holds the raw block data in memory
    bool dirty; // set if the block was written to and needs flushing
    bool valid; // marks whether this entry currently holds valid data
    unsigned int access_time; // timestamp used for LRU eviction
    bool in_use; // true if this block is currently pinned or in active use

    // struct condition wait_cv; 
    int owner_tid;  
    int waiters;  //number of waiters for this entry
};


struct cache{
    struct storage *stor;// pointer to the backing storage device
    unsigned int block_size; // cache block size, usually equals CACHE_BLKSZ
    struct cache_entry *entries;  // main cache storage, fixed 64 entries
    

    unsigned int timer;// simple time counter for LRU tracking
    int last_used; // index of the most recently returned cache block
    struct lock mtx;
    struct condition any_cv;  
};

/**
 * @brief Creates/initializes a cache with the passed backing storage device (disk) and makes it
 * available through cptr.
 * @param disk Pointer to the backing storage device.
 * @param cptr Pointer to the cache to create.
 * @return 0 on success, negative error code if error
 */
int create_cache(struct storage* disk, struct cache** cptr) {
    // create and initialize a cache for the given storage device

    if(!disk || !cptr) return -EINVAL; // check for invalid pointers
    if (storage_blksz(disk) != CACHE_BLKSZ)  return -EINVAL; // make sure block size matches expected size

    struct cache *cache = kcalloc(1, sizeof(*cache)); // allocate and zero memory for cache
    if (!cache) return -ENOMEM; // allocation failed

    cache->stor = disk; // link cache to its backing storage
    cache->block_size = CACHE_BLKSZ; // set block size
    cache->timer = 0; // initialize timer used for LRU tracking
    cache->last_used = -1; // no previous block marked as used yet
    lock_init(&cache->mtx); //cache initialization

    condition_init(&cache->any_cv, "cache_any_wait");
    cache->entries = kcalloc(64, sizeof(*cache->entries));
    if (!cache->entries) { kfree(cache); return -ENOMEM; }


    for(int i = 0; i<64;i++){ // loop through each cache entry and initialize
        cache->entries[i].block_n = 0; // no block assigned yet
        cache->entries[i].valid = false; // mark entry as invalid
        cache->entries[i].dirty = false; // not dirty, clean state
        cache->entries[i].in_use = false; // not pinned or actively used
        cache->entries[i].access_time = 0; // no access time set

        cache->entries[i].data = kmalloc(CACHE_BLKSZ);
        if (!cache->entries[i].data) {
            // roll back previous allocations, then fail
            for (int j = 0; j < i; j++) {
                if (cache->entries[j].data) kfree(cache->entries[j].data);
                cache->entries[j].data = NULL;
            }
            kfree(cache->entries);
            kfree(cache);
            return -ENOMEM;
        }

        cache->entries[i].owner_tid = -1;   // FIX: no owner initially
        cache->entries[i].waiters = 0; 

    }

    *cptr = cache; // return initialized cache structure to caller

    return 0; // success
}



/**
 * @brief Reads a CACHE_BLKSZ sized block from the backing interface into the cache.
 * @param cache Pointer to the cache.
 * @param pos Position in the backing storage device. Must be aligned to a multiple of the block
 * size of the backing interface.
 * @param pptr Pointer to the block pointer read from the cache. Assume that CACHE_BLKSZ will always
 * be equal to the block size of the storage disk. Any replacement policy is permitted, as long as
 * your design meets the above specifications.
 * @return 0 on success, negative error code if error
 */
int cache_get_block(struct cache* cache, unsigned long long pos, void** pptr) {
    // FIXME
    // char* buff = NULL;

    if (!cache || !cache->stor || !pptr) return -EINVAL; // quick arg validation: need cache, storage, and output ptr
    if (pos % CACHE_BLKSZ != 0) return -EINVAL; // enforce block alignment for pos

    lock_acquire(&cache->mtx);

    unsigned long long position = pos/CACHE_BLKSZ; // convert byte offset to block index

    //previously returned cache stayes in use until next call
    if(cache->last_used >= 0 && cache->entries[cache->last_used].owner_tid == running_thread()) { // unpin the last block we handed out 
        cache->entries[cache->last_used].in_use = false;
        cache->entries[cache->last_used].owner_tid = -1;
        condition_broadcast(&cache->any_cv); 
        cache->last_used = -1;
    }

    //hit check
    for(int i = 0;i<64; i++){
        if(cache->entries[i].block_n ==(unsigned int)position && cache->entries[i].valid == true){
            while (cache->entries[i].in_use && cache->entries[i].owner_tid != running_thread()) {
                cache->entries[i].waiters += 1; 
                lock_release(&cache->mtx);
                condition_wait(&cache->any_cv);  
                lock_acquire(&cache->mtx);  
                cache->entries[i].waiters -= 1; 
            }

            *pptr = cache->entries[i].data; // hand caller a direct pointer into cache
            cache->entries[i].in_use = true;// pin this entry until next get_block()
            cache->entries[i].owner_tid = running_thread(); 
            cache->timer+=1; // bump access clock (for LRU-ish policy)
            cache->entries[i].access_time = cache->timer; // record recency
            
            cache->last_used = i; // remember which entry is pinned
            
            lock_release(&cache->mtx); 
            return 0; // cache hit
        }
    }

    //miss check

    int index = cache_evict_entry(cache); // pick a victim (or free slot)
    if (index < 0){ 
        lock_release(&cache->mtx); 
        return -EBUSY; // couldn't find an evictable slot
    }
    //writeback dirty entry if dirty
    if (cache->entries[index].valid && cache->entries[index].dirty) {
        unsigned long long evict_pos = (unsigned long long)cache->entries[index].block_n * CACHE_BLKSZ; // compute byte offset of victim
        long ret = storage_store(cache->stor, evict_pos, cache->entries[index].data, CACHE_BLKSZ); // write back full block
        if (ret < 0){
            lock_release(&cache->mtx); 
            return ret; 
        }
        if ((unsigned long)ret != CACHE_BLKSZ){
            lock_release(&cache->mtx); 
            return -EIO;  // short write => I/O error
        }
        cache->entries[index].dirty = false; // now clean
    }


    long ret = storage_fetch(cache->stor, pos, cache->entries[index].data, CACHE_BLKSZ); // read new block from disk
    if (ret < 0){
        lock_release(&cache->mtx); 
        return ret;
    }
    if((unsigned long)ret != CACHE_BLKSZ){
        lock_release(&cache->mtx); 
        return -EIO; // short read => I/O error
    }

    cache->entries[index].block_n = (unsigned int)position; // tag with block number
    cache->entries[index].valid = true; // mark as valid
    cache->entries[index].dirty = false; // freshly fetched => clean
    cache->entries[index].in_use = true; // pin until next call
    cache->entries[index].owner_tid = running_thread();
    cache->timer+=1; // update recency clock
    cache->entries[index].access_time =cache->timer; // set access time
    cache->last_used =index; // remember pinned slot

    *pptr = cache->entries[index].data; // return pointer to cached block

    // cache->entries[index].data = *buff;
    lock_release(&cache->mtx); 
    return 0; // miss handled successfully
}


/**
 * @brief Releases a block previously obtained from cache_get_block().
 * @param cache Pointer to the cache.
 * @param pblk Pointer to a block that was made available in cache_get_block() (which means that
 * pblk == *pptr for some pptr).
 * @param dirty Indicates whether the block has been modified (1) or not (0). If dirty == 1, the
 * block has been written to. If dirty == 0, the block has not been written to.
 * @return 0 on success, negative error code if error
 */
void cache_release_block(struct cache* cache, void* pblk, int dirty) {
    // FIXME

    if (!cache || !pblk) return; // invalid inputs, nothing to do

    lock_acquire(&cache->mtx);

    

    // locate the cache entry that matches the provided pointer
    for (int i = 0; i < 64; i++) {
        if (cache->entries[i].valid && cache->entries[i].data == pblk) {
            if(dirty){ // if caller modified the block, mark it dirty
                cache->entries[i].dirty = true;
            }

            cache->entries[i].in_use = false;     
            cache->entries[i].owner_tid = -1;             
            condition_broadcast(&cache->any_cv); 
            lock_release(&cache->mtx);
            return; // done after marking dirty (no unpin here — handled on next get_block())
        }
    }


    lock_release(&cache->mtx);
    return; // pointer didn't match any entry; ignore silently
}

/**
 * @brief Flushes the cache to the backing device
 * @param cache Pointer to the cache to flush
 * @return 0 on success, error code if error
 */
int cache_flush(struct cache* cache) {
    // writes all dirty blocks in the cache back to the storage device

    if(!cache || !cache->stor) return -EINVAL; // check for invalid cache or missing backing store

    lock_acquire(&cache->mtx);


    for(int i=0; i<64; i++){ // iterate through all cache entries
        if(cache->entries[i].valid && cache->entries[i].dirty){ // only flush valid dirty blocks
            unsigned long long evict_pos = (unsigned long long)cache->entries[i].block_n * CACHE_BLKSZ; // compute byte offset
            long ret = storage_store(cache->stor, evict_pos, cache->entries[i].data, CACHE_BLKSZ); // write block back to disk
            if (ret < 0){
                lock_release(&cache->mtx);
                return ret; // write failure
            }
            if ((unsigned long)ret != CACHE_BLKSZ){
                lock_release(&cache->mtx);
                return -EIO; // incomplete write
            }
            cache->entries[i].dirty = false; // mark entry as clean after flush
        }
    }
    lock_release(&cache->mtx);
    return 0; // success — all dirty blocks flushed
}

int cache_evict_entry(struct cache* cache){
    unsigned int min = ~0u; // start with max possible unsigned value for comparison
    int ret_index = -1; // index of chosen entry to evict, -1 means none found

    //first search for empty entries
    for (int i = 0; i < 64; i++) {
        if (!cache->entries[i].valid && !cache->entries[i].in_use) { // free and not pinned
            return i; // immediately return first free slot
        }
    }

    //next search for filled entries to evict
    for(int i = 0; i<64; i++){
        if (cache->entries[i].valid && !cache->entries[i].in_use) { // consider only valid and unpinned blocks
            if(ret_index == -1 || cache->entries[i].access_time < min){ // pick least recently used
                min = cache->entries[i].access_time; // update minimum timestamp
                ret_index = i; // record candidate index
            }
        }
    }

    return ret_index; // return LRU candidate, or -1 if all entries are pinned
    
}