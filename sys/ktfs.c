/*! @file ktfs.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‌‌​‌‌‌‌​⁠⁠‌
    @brief KTFS Implementation.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif

#include "ktfs.h"

#include "cache.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "filesys.h"
#include "fsimpl.h"
#include "heap.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"
#include "uioimpl.h"
#include <stdbool.h>




struct ktfs_mount {
    struct filesystem fs; // filesystem ops table + identity for this mount
    struct cache* cache;  // backing block cache we read/write through
    struct lock mount_lock; //mount lock
};


// INTERNAL TYPE DEFINITIONS
//

/// @brief File struct for a file in the Keegan Teal Filesystem
struct ktfs_file {
    // Fill to fulfill spec
    struct ktfs_mount* fs; // link back to the filesystem mount
    uint64_t size; // total size of the file in bytes
    uint64_t position; // current read/write cursor position
    uint64_t offset; // optional offset field for internal use
};

struct ktfs_uio {
    struct uio base; // base uio structure with interface and metadata
    struct ktfs_file file; // contains per-file info like size and position
    uint16_t inode_number; // inode identifier for the open file
    struct lock file_lock; //file lock
};

struct ktfs_listing_uio {
    struct uio base;
    struct ktfs_mount *mount;
    struct ktfs_superblock super;
    struct ktfs_inode root;
    uint32_t next_index;
    uint32_t total_entries;
};



// INTERNAL FUNCTION DECLARATIONS
//

static int ktfs_read_super(struct ktfs_mount* mount, struct ktfs_superblock* super);
int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr);
void ktfs_close(struct uio* uio);
int ktfs_cntl(struct uio* uio, int cmd, void* arg);
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len);
long ktfs_store(struct uio* uio, const void* buf, unsigned long len);
int ktfs_create(struct filesystem* fs, const char* name);
int ktfs_delete(struct filesystem* fs, const char* name);
void ktfs_flush(struct filesystem* fs);

void ktfs_listing_close(struct uio* uio);
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz);

static void ktfs_compute_layout(const struct ktfs_superblock* sb,
                                uint32_t* inode_bitmap_start,
                                uint32_t* block_bitmap_start,
                                uint32_t* inode_start,
                                uint32_t* data_start);
static int ktfs_map_block(struct ktfs_mount* m, const struct ktfs_superblock* sb,
                              const struct ktfs_inode* ino, uint32_t lbn, uint32_t* absblk);
static int ktfs_dir_get_entry(struct ktfs_mount* m, const struct ktfs_superblock* sb,
                              const struct ktfs_inode* dir_ino, uint32_t idx,
                              struct ktfs_dir_entry* out);
static int ktfs_inode_grab(struct ktfs_mount* mount, uint32_t inode_num,
                           const struct ktfs_superblock* super, struct ktfs_inode* dest);

static int ktfs_bitmap_indices_fetch(const struct ktfs_superblock* sb, int bitmap_id, uint32_t* blk_start_out, uint32_t* blk_cnt_out,
                                  uint32_t* total_bits_out);

static int ktfs_bitmap_free_bit_detect(struct ktfs_mount* m, const struct ktfs_superblock* sb, int kind, uint32_t* outIndex);

static int ktfs_bitmap_mark(struct ktfs_mount* m, const struct ktfs_superblock* superb, int kind, uint32_t index);

static int ktfs_bitmap_unmark(struct ktfs_mount* m, const struct ktfs_superblock* superb, int kind,
                              uint32_t index);

static int ktfs_write_to_ino(struct ktfs_mount* mount, uint32_t inode_num, const struct ktfs_superblock* superb,
                            const struct ktfs_inode* src);

static int ktfs_inode_free_all_blocks(struct ktfs_mount* mount, const struct ktfs_superblock* superb, struct ktfs_inode* ino);

static int ktfs_map_block_and_or_allocate(struct ktfs_mount* mount, const struct ktfs_superblock* superb, struct ktfs_inode* ino, uint32_t lbn, uint32_t* absblk, int allocatE);

static int ktfs_alloc_zero_block(struct ktfs_mount* m, const struct ktfs_superblock* sb, uint32_t* out_abs);


static const struct uio_intf ktfs_uio_intf = {
    .close= ktfs_close,
    .read= ktfs_fetch,
    .write = ktfs_store,      
    .cntl = ktfs_cntl
};

static const struct uio_intf ktfs_listing_uio_intf = {
    .close = ktfs_listing_close,
    .read  = ktfs_listing_read,
};



/**
 * @brief Mounts the file system with associated backing cache
 * @param cache Pointer to cache struct for the file system
 * @return 0 if mount successful, negative error code if error
 */
int mount_ktfs(const char* name, struct cache* cache) {
    // FIXME
    struct ktfs_mount* mount; // mount structure to hold filesystem + cache

    // Validate input parameters
    if(!name || !*name || !cache){ // check for missing or invalid args
        return -EINVAL; // invalid input
    }

    // Allocate memory for mount structure
    mount = kcalloc(1, sizeof(*mount)); // allocate and zero-initialize mount
    if(!mount){ // handle allocation failure
        return -ENOMEM; // out of memory
    }

    lock_init(&mount->mount_lock);  //aquire mount_lock

    

    //Initialize filesystem functions
    mount->fs.open =ktfs_open; // assign open handler
    mount->fs.create = ktfs_create; // create not supported
    mount->fs.delete = ktfs_delete; // delete not supported
    mount->fs.flush = ktfs_flush; // set flush handler (optional)
    mount->cache= cache; // store provided cache pointer

    //attach the filesystem
    int ret = attach_filesystem(name, &mount->fs); // register filesystem
    if(ret < 0){ // check if registration failed
        kfree(mount); // free allocated mount
        return ret; // return the error code
    }

    return 0; // mount successful
}

/**
 * @brief Opens a file or ls (listing) with the given name and returns a pointer to the uio through
 * the double pointer
 * @param name The name of the file to open or "\" for listing (CP3)
 * @param uioptr Will return a pointer to a file or ls (list) uio pointer through this double
 * pointer
 * @return 0 if open successful, negative error code if error
 */
int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr) {
    // FIXME
    struct ktfs_mount *mount = (struct ktfs_mount *)fs; // cast to mount to access cache
    
    if(!fs || !uioptr || !name || !*name){ // validate input arguments
        return -EINVAL; // invalid parameters
    }

    const char *n = name; // local name for control flow
    bool is_listing = false; // track whether this is a listing open

    if(n == NULL || *n == '\0'){ 
        is_listing = true; // listing requested
    }
    else if(n[0] == '/'){ 
        n++; // skip leading slash
        if(*n == '\0'){ // path is exactly "/" so list the root directory
            is_listing = true; // listing requested
        }
    }

    if(is_listing){
        struct ktfs_listing_uio *ls = kcalloc(1, sizeof(*ls)); // allocate listing uio structure
        if(!ls){
            return -ENOMEM; // fail if allocation for listing uio fails
        }

        ls->mount= mount; // remember which mount this listing belongs to
        ls->next_index = 0; // start from the first directory entry on read

        lock_acquire(&mount->mount_lock); // serialize snapshot of root listing state

        int ret = ktfs_read_super(mount, &ls->super); // read superblock for listing
        if(ret < 0){
            lock_release(&mount->mount_lock);
            kfree(ls);
            return ret;
        }

        ret = ktfs_inode_grab(mount, ls->super.root_directory_inode, &ls->super, &ls->root); // load root inode
        if(ret < 0){
            lock_release(&mount->mount_lock);
            kfree(ls);
            return ret;
        }

        uint32_t entry_sz = sizeof(struct ktfs_dir_entry); // size of one dir entry
        if((ls->root.size % entry_sz) != 0){
            lock_release(&mount->mount_lock);
            kfree(ls);
            return -EIO; // corrupt directory size
        }

        ls->total_entries = ls->root.size / entry_sz; // total entries for listing traversal

        lock_release(&mount->mount_lock); // done snapshot

        *uioptr = uio_init1(&ls->base, &ktfs_listing_uio_intf); // initialize uio with listing interface
        return 0; // listing open successful
    }

    name = n;


    void*superb = NULL; // pointer to hold superblock data
    int ret = cache_get_block(mount->cache, 0UL, &superb); // read superblock from cache
    if(ret<0) return ret; // fail if unable to fetch

    struct ktfs_superblock* super = (struct ktfs_superblock*)superb; // interpret as superblock

    uint32_t inode_bitmap_start= 0; // track inode bitmap start
    uint32_t block_bitmap_start=0;  // track block bitmap start
    uint32_t inode_start=0; // start of inode table
    uint32_t data_start=0; // start of data blocks
    
    ktfs_compute_layout(super, &inode_bitmap_start, &block_bitmap_start, &inode_start, &data_start); // compute layout offsets
    

    struct ktfs_inode root_inode; // holds root directory inode

    ret = ktfs_inode_grab(mount, super->root_directory_inode, super, &root_inode); // load root inode
    if(ret<0){ 
        cache_release_block(mount->cache, superb, 0); // release block if failure
        return ret; 
    }

    kprintf("[KTFS] sb: blocks=%u ibmap=%u bmap=%u iblocks=%u root=%u\n",
        super->block_count,
        super->inode_bitmap_block_count,
        super->bitmap_block_count,
        super->inode_block_count,
        super->root_directory_inode);

    kprintf("[KTFS] root.size=%u (bytes)\n", root_inode.size);

    int total_entries = root_inode.size / sizeof(struct ktfs_dir_entry); // total entries in directory
    struct ktfs_dir_entry entry; // holds directory entry during iteration
    uint16_t file_inode= 0; // inode number for matching file
    kprintf("[KTFS] open '%s'\n", name);
    bool found = false;
    for(int i = 0; i<total_entries; i++){ // iterate through directory entries
        int rt=ktfs_dir_get_entry(mount, super, &root_inode, i, &entry); // fetch entry i
        if(rt<0){
            if(rt == -ENOENT) continue; // skip empty entries
            cache_release_block(mount->cache, superb, 0); // release block if hard error
            return rt;
        }

        
        entry.name[KTFS_MAX_FILENAME_LEN] = '\0'; // ensure null termination
        kprintf("[KTFS] entry %d: inode=%d name='%s'\n", i, entry.inode, entry.name);

        if(strncmp(entry.name, name, KTFS_MAX_FILENAME_LEN) == 0){ // compare names
            file_inode = entry.inode; // match found, store inode
            found = true;
            break;
        }
    }

    if(!found) { // file not found
        cache_release_block(mount->cache, superb, 0); 
        return -ENOENT; 
    }

    struct ktfs_inode target_inode; // inode of target file

    ret = ktfs_inode_grab(mount, file_inode, super, &target_inode); // load target inode
    cache_release_block(mount->cache, superb, 0); // done with superblock
    if(ret<0)return ret;

    struct ktfs_uio* ku = kcalloc(1, sizeof(*ku)); // allocate uio structure
    if(!ku) return -ENOMEM; // check memory allocation

    lock_init(&ku->file_lock); //accquire file_lock

    ku->file.fs = mount; // link file to mount
    ku->file.size= target_inode.size; // copy file size
    ku->file.position = 0; // start position at beginning
    ku->file.offset = 0; // no offset initially
    ku->inode_number= file_inode; // store inode id

    *uioptr = uio_init1(&ku->base, &ktfs_uio_intf); // initialize and return uio
    return 0; // success
}

/**
 * @brief Closes the file that is represented by the uio struct
 * @param uio The file io to be closed
 * @return None
 */
void ktfs_close(struct uio* uio) {
    struct ktfs_uio* x = (struct ktfs_uio*)uio;
    kfree(x); // free the allocated memory for this uio structure
}

/**
 * @brief Reads data from file attached to uio into provided argument buffer
 * @param uio uio of file to be read
 * @param buf Buffer to be filled
 * @param len Number of bytes to read
 * @return Number of bytes read if successful, negative error code if error
 */
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len) {
    // FIXME
    if(len == 0) return 0;// nothing to do; quick exit
    if(buf == NULL && len > 0) return -EINVAL; // guard: must have a buffer if len > 0
    struct ktfs_uio *kuio = (struct ktfs_uio *)uio; // downcast to our concrete uio

    lock_acquire(&kuio->file_lock); //acquire file_lock

    struct ktfs_mount* mount = kuio->file.fs; // cached mount (cache + fs hooks)
    struct ktfs_file file = kuio->file; // snapshot of per-open file state
    uint64_t size = file.size; // initial size (will refresh from inode below)
    // uint64_t offset = file.offset; // offset not used yet; kept for spec
    uint64_t pos = file.position; // current file position
    
    struct ktfs_superblock superb;
    int ret = ktfs_read_super(mount, &superb); // read superblock via cache
    if(ret<0){ 
        lock_release(&kuio->file_lock); 
        return ret; 
    }  

    struct ktfs_inode inode;
    ret = ktfs_inode_grab(mount, kuio->inode_number,  &superb, &inode); // fetch latest inode
    if(ret<0) { 
        lock_release(&kuio->file_lock); 
        return ret;
    }

    size = inode.size; // refresh size from on-disk inode
    kuio->file.size = inode.size;
    if(pos>=size){
        lock_release(&kuio->file_lock); 
        return 0;// at or past EOF 
    }


    uint64_t read_size = MIN((uint64_t)len, size - pos); //clamp to remaining bytes in file

    // uint32_t inode_bitmap_start= 0;
    // uint32_t block_bitmap_start=0;
    // uint32_t inode_start=0;
    // uint32_t data_start=0;
    // ktfs_compute_layout(superb, &inode_bitmap_start, &block_bitmap_start, &inode_start, &data_start);
    
    uint64_t copied = 0; // running total
    uint32_t absblk=0; // physical (absolute) block number on disk
    while(copied<read_size){
        uint64_t lbn= (copied+pos)/KTFS_BLKSZ; // logical block index within file
        uint64_t within_block_off = (copied+pos) % KTFS_BLKSZ; // offset into that block
        uint64_t width = MIN(read_size-copied, KTFS_BLKSZ - within_block_off); // bytes from this block
        ret = ktfs_map_block(mount, &superb, &inode, (uint32_t)lbn, &absblk); // map LBN→disk block

        if(ret == -ENOENT){//handle sparse hole by zero-fill
            memset((uint8_t*)buf + copied, 0, (size_t)width); // fill hole with zeros
            copied += width;
            continue; // move on to next region
        }

        if(ret<0){ lock_release(&kuio->file_lock); return ret; } // real error from mapper
        void*block = NULL;
        ret = cache_get_block(mount->cache, (unsigned long long)absblk * KTFS_BLKSZ, &block); // pull block into cache
        if(ret<0) { 
            lock_release(&kuio->file_lock); 
            return ret; 
        }
        memcpy((uint8_t*)buf + copied, (uint8_t*)block + within_block_off, (size_t)width); // copy slice out
        cache_release_block(mount->cache, block, 0); // release (clean) cache line

        copied+=width; // advance progress
        

    }

    kuio->file.position+=copied; // advance file pointer for this handle
    lock_release(&kuio->file_lock); //release lock;

    return (long )copied; // bytes read


    
}

/**
 * @brief Write data from the provided argument buffer into file attached to uio
 * @param uio The file to be written to
 * @param buf The buffer to be read from
 * @param len Number of bytes to write from the buffer to the file
 * @return Number of bytes written from the buffer to the file system if sucessful, negative error
 * code if error
 */
long ktfs_store(struct uio* uio, const void* buf, unsigned long len) {
    // FIXME
    if(!uio) {
        return -EINVAL;
    }
    if(len == 0) {
        return 0;
    }
    if(buf == NULL) {
        return -EINVAL;
    }

    struct ktfs_uio* kuio = (struct ktfs_uio*)uio;
    struct ktfs_mount* mount = kuio->file.fs; // pull mount from file handle

    lock_acquire(&mount->mount_lock);
    lock_acquire(&kuio->file_lock); // locks

    struct ktfs_superblock superb;
    int ret = ktfs_read_super(mount, &superb); // read super to know layout and limits
    if(ret < 0){
        lock_release(&kuio->file_lock);
        lock_release(&mount->mount_lock);

        return ret;
    }

    struct ktfs_inode inode;
    ret = ktfs_inode_grab(mount, kuio->inode_number, &superb, &inode); // fetch in-memory inode copy
    if(ret < 0){
        lock_release(&kuio->file_lock);
        lock_release(&mount->mount_lock);
        return ret;
    }

    uint64_t pos = kuio->file.position;
    uint64_t size = inode.size; // snapshot file size before write

    if(pos >= KTFS_MAX_FILE_SIZE) {
        lock_release(&kuio->file_lock);
        lock_release(&mount->mount_lock);
        return -EINVAL; // refuse writes that start past the max file size
    }

    uint64_t max_writable = KTFS_MAX_FILE_SIZE - pos; // remaining headroom to the cap
    uint64_t size_of_write = (uint64_t)len;

    if(size_of_write > max_writable) {
        size_of_write = max_writable; // clamp request to allowed window
    }

    if(size_of_write == 0) {
        lock_release(&kuio->file_lock);
        lock_release(&mount->mount_lock);
        // return -EINVAL;
        return 0; // nothing to do after clamp
    }

    uint64_t write_end = pos + size_of_write; // exclusive end offset of this write

    if(write_end > size) {
        uint64_t old_size = size;
        uint64_t olderBlks;
        if(old_size == 0) {

            olderBlks = 0; // no previously allocated data blocks
        } 
        else{


            olderBlks = (old_size + KTFS_BLKSZ - 1) / KTFS_BLKSZ; // number of blocks currently covering size
        }

        uint64_t new_blocks = (write_end + KTFS_BLKSZ - 1) / KTFS_BLKSZ; // blocks needed after growth

        for(uint32_t x = (uint32_t)olderBlks; x < (uint32_t)new_blocks; x++) {
            uint32_t absblk_tmp = 0;
            ret = ktfs_map_block_and_or_allocate(mount, &superb, &inode, x, &absblk_tmp, 1); // allocate and map new lbn

            if(ret < 0){
                lock_release(&kuio->file_lock);
                lock_release(&mount->mount_lock);

                return ret;
            }
        }
    }
    uint64_t fOffset = pos; // running file offset during copy

    while(fOffset < write_end){
        uint32_t lbn = (uint32_t)(fOffset / KTFS_BLKSZ);
        uint32_t off_in_block = (uint32_t)(fOffset % KTFS_BLKSZ); // in-block start

        uint64_t what_remains = write_end - fOffset;
        uint32_t chunk = (uint32_t)MIN(what_remains, (uint64_t)(KTFS_BLKSZ - off_in_block)); // bytes to write this round

        uint32_t absblk = 0;
        ret = ktfs_map_block_and_or_allocate(mount, &superb, &inode, lbn, &absblk, 1); // ensure mapping for target block
        if(ret < 0){
            lock_release(&kuio->file_lock);
            lock_release(&mount->mount_lock);
            return ret;
        }

        void* blk = NULL;
        ret = cache_get_block(mount->cache, (unsigned long long)absblk * KTFS_BLKSZ, &blk); // pin cache line for the block
        if(ret < 0) {
            lock_release(&kuio->file_lock);
            lock_release(&mount->mount_lock);
            return ret;
        }

        memcpy((uint8_t*)blk + off_in_block, (const uint8_t*)buf + (fOffset - pos), chunk); // write payload into cache
        cache_release_block(mount->cache, blk, 1); // set dirty to schedule writeback

        fOffset += chunk; // advance cursor
    }

    uint64_t new_end = fOffset;
    if(new_end > size){
        inode.size = (uint32_t)new_end; // grow logical file size
    }

    ret = ktfs_write_to_ino(mount, kuio->inode_number, &superb, &inode); // persist updated inode to disk
    if(ret < 0){
        lock_release(&kuio->file_lock);
        lock_release(&mount->mount_lock);
        return ret;
    }

    kuio->file.size = inode.size; // refresh in-memory file metadata
    kuio->file.position = new_end; // final position after write

    lock_release(&kuio->file_lock);
    lock_release(&mount->mount_lock); // end critical section
    return (long)(new_end - pos); // number of bytes actually written
}


/**
 * @brief Create a new file in the file system
 * @param fs The file system in which to create the file
 * @param name The name of the file
 * @return 0 if successful, negative error code if error
 */
int ktfs_create(struct filesystem* fs, const char* name) {
    // FIXME
    if(!fs || !name || !*name) {
        return -EINVAL; // reject bad inputs early
    }

    struct ktfs_mount* mount = (struct ktfs_mount*)fs; // cast to concrete mount

    lock_acquire(&mount->mount_lock); // serialize namespace ops

    if(name[0] == '/') {
        name++; // drop leading slash for simple names
        if(*name == '\0') {
            lock_release(&mount->mount_lock);
            return -EINVAL; // path was only a slash
        }
    }

    struct ktfs_superblock superb;
    int ret = ktfs_read_super(mount, &superb); // load superblock for layout
    if(ret < 0) {
        lock_release(&mount->mount_lock);
        return ret;
    }

    struct ktfs_inode root;
    ret = ktfs_inode_grab(mount, superb.root_directory_inode, &superb, &root); // fetch root inode
    if(ret < 0) {
        lock_release(&mount->mount_lock);
        return ret;
    }

    uint32_t entry_size = sizeof(struct ktfs_dir_entry);
    if(root.size % entry_size != 0) {
        lock_release(&mount->mount_lock);
        return -EIO; // directory not block aligned
    }

    uint32_t nents = root.size / entry_size; // number of entries currently in root
    struct ktfs_dir_entry dent;

    // DEBUG for shell
    // Need to move the old entries into a newly allocated dir
    // So find the max number of entries for the size of the array
    const uint32_t max_dir_ents = KTFS_BLKSZ / entry_size;
    struct ktfs_dir_entry old_entries[max_dir_ents];
    
    // Create a counter too
    uint32_t old_count = 0;

    uint32_t i = 0;
    int seen = 0;
    while(i < nents) {
        ret = ktfs_dir_get_entry(mount, &superb, &root, i, &dent); // pull entry i
        if(ret == -ENOENT) {
            i++;
            continue; // hole in directory table
        }
        if(ret < 0) {
            lock_release(&mount->mount_lock);
            return ret;
        }

        dent.name[KTFS_MAX_FILENAME_LEN] = '\0'; // ensure safe compare
        if(strncmp(dent.name, name, KTFS_MAX_FILENAME_LEN) == 0) {
            seen = 1; // already exists
            break;
        }

        // Remember the existing entries to move into the data block
        if(old_count < max_dir_ents){

            old_entries[old_count++] = dent;
        }
        i++;
    }

    // Make the unused calls
    (void) old_entries;
    (void) old_count;

    if(seen){
        lock_release(&mount->mount_lock);
        return -EEXIST; // name collision
    }

    uint32_t new_ino_num = 0;
    ret = ktfs_bitmap_free_bit_detect(mount, &superb, 0, &new_ino_num); // grab free inode id
    if(ret < 0) {
        lock_release(&mount->mount_lock);
        return ret;
    }

    ret = ktfs_bitmap_mark(mount, &superb, 0, new_ino_num); // mark inode in use
    if(ret < 0) {
        lock_release(&mount->mount_lock);
        return ret;
    }

    struct ktfs_inode newino;
    memset(&newino, 0, sizeof(newino)); // zero new inode fields
    newino.size = 0; // empty file

    ret = ktfs_write_to_ino(mount, new_ino_num, &superb, &newino); // persist inode
    if(ret < 0) {
        lock_release(&mount->mount_lock);
        return ret;
    }

    struct ktfs_dir_entry newdent;
    memset(&newdent, 0, sizeof(newdent)); // prep directory entry
    newdent.inode = (uint16_t)new_ino_num; // link to new inode
    strncpy(newdent.name, name, KTFS_MAX_FILENAME_LEN); // copy name into slot
    newdent.name[KTFS_MAX_FILENAME_LEN] = '\0'; // force terminator

    uint64_t dir_off = root.size; // append at end of directory
    uint32_t lbn = (uint32_t)(dir_off / KTFS_BLKSZ);
    uint32_t off_in_block = (uint32_t)(dir_off % KTFS_BLKSZ);
    uint32_t absblk = 0;

    

    // Check

    // Need to move var
    int need_move = 0;

    // Initially try to map to an existing directory without forcing a new allocation
    ret = ktfs_map_block_and_or_allocate(mount, &superb, &root, lbn, &absblk, 0);
    
    // Change structure of this
    // If this returns as an error, then there are no blocks mapped yet so now allocate one
    if(ret == -ENOENT){

        ret = ktfs_map_block_and_or_allocate(mount, &superb, &root, lbn, &absblk, 1); // ensure dir block exists
    
        // DEBUG: move inside the error loop
        if(ret < 0) {
            lock_release(&mount->mount_lock);
            return ret;
        }

        // Set the need move to 1
        need_move = 1;
    }

    // Create an if else in case to not lose that functionality
    else if (ret < 0) {
        lock_release(&mount->mount_lock);
        return ret;
    }

    // Unused call
    (void) need_move;

    void* blk = NULL;
    ret = cache_get_block(mount->cache, (unsigned long long)absblk * KTFS_BLKSZ, &blk); // get cache line for dir block
    if(ret < 0){
        lock_release(&mount->mount_lock);
        return ret;
    }

    memcpy((uint8_t*)blk + off_in_block, &newdent, sizeof(newdent)); // write entry payload
    cache_release_block(mount->cache, blk, 1); // dirty it for writeback

    root.size += sizeof(newdent); // advance directory size

    ret = ktfs_write_to_ino(mount, superb.root_directory_inode, &superb, &root); // persist root update
    if(ret < 0) {
        lock_release(&mount->mount_lock);
        return ret;
    }

    lock_release(&mount->mount_lock); // unlock 

    return 0; // success
}


/**
 * @brief Deletes a certain file from the file system with the given name
 * @param fs The file system to delete the file from
 * @param name The name of the file to be deleted
 * @return 0 if successful, negative error code if error
 */
int ktfs_delete(struct filesystem* fs, const char* name) {
    //FIXME

    if(!fs || !name || !*name){
        return -EINVAL;
    } // basic arg validation

    if(name[0] == '/') {
        name++;
        if(!*name) {
            return -EINVAL;
        }
    } // trim leading slash for absolute paths, reject root

    struct ktfs_mount* mount = (struct ktfs_mount*)fs;
    lock_acquire(&mount->mount_lock); // serialize directory and bitmap updates

    struct ktfs_superblock superb;
    int ret = ktfs_read_super(mount, &superb);
    if(ret < 0){
        lock_release(&mount->mount_lock);
        return ret;
    } // load superblock for layout and root inode id

    struct ktfs_inode root;
    ret = ktfs_inode_grab(mount, superb.root_directory_inode, &superb, &root);
    if(ret < 0) {
        lock_release(&mount->mount_lock);
        return ret;
    } // read root directory inode

    const uint32_t entry_sz = sizeof(struct ktfs_dir_entry);
    if((root.size % entry_sz) != 0) {
        lock_release(&mount->mount_lock);
        return -EIO;
    } // directory size must be multiple of entry size

    uint32_t nents = root.size / entry_sz;
    if(nents == 0){
        lock_release(&mount->mount_lock);
        return -ENOENT;
    } // nothing to delete if directory is empty

    uint32_t victim_idx = (uint32_t)-1;
    uint32_t victim_ino = 0;
    struct ktfs_dir_entry dent; // scratch for scanning

    uint32_t i = 0;
    while(i < nents){
        ret = ktfs_dir_get_entry(mount, &superb, &root, i, &dent);
        if(ret == -ENOENT) {
            i++;
            continue;
        } // skip holes left by prior deletes
        if(ret < 0) {
            lock_release(&mount->mount_lock);
            return ret;
        } // propagate read failure

        dent.name[KTFS_MAX_FILENAME_LEN] = '\0'; // hard stop name to avoid overread
        if(strncmp(dent.name, name, KTFS_MAX_FILENAME_LEN) == 0) {
            victim_idx = i;
            victim_ino = dent.inode;
            break;
        } // found target entry
        i++;
    } // linear search across directory entries

    if(victim_idx == (uint32_t)-1) {
        lock_release(&mount->mount_lock);
        return -ENOENT;
    } // not found

    struct ktfs_inode victim;
    ret = ktfs_inode_grab(mount, victim_ino, &superb, &victim);
    if(ret < 0){
        lock_release(&mount->mount_lock);
        return ret;
    } // load inode to free its blocks

    ret = ktfs_inode_free_all_blocks(mount, &superb, &victim);
    if(ret < 0) {
        lock_release(&mount->mount_lock);
        return ret;
    } // release direct indirect and dindirect data

    ret = ktfs_write_to_ino(mount, victim_ino, &superb, &victim);
    if(ret < 0) {
        lock_release(&mount->mount_lock);
        return ret;
    } // persist cleared inode

    ret = ktfs_bitmap_unmark(mount, &superb, 0, victim_ino);
    if(ret < 0) {
        lock_release(&mount->mount_lock);
        return ret;
    } // free the inode number in inode bitmap

    uint32_t last_idx = nents - 1;
    if(victim_idx != last_idx) {
        struct ktfs_dir_entry last;
        ret = ktfs_dir_get_entry(mount, &superb, &root, last_idx, &last);
        if(ret == -ENOENT) {
            lock_release(&mount->mount_lock);
            return -EIO;
        }
        if(ret < 0) {
            lock_release(&mount->mount_lock);
            return ret;
        } // fetch last entry for swap delete

        uint64_t victim_off = (uint64_t)victim_idx * entry_sz;
        uint32_t v_lbn = (uint32_t)(victim_off / KTFS_BLKSZ);
        uint32_t v_off = (uint32_t)(victim_off % KTFS_BLKSZ);
        uint32_t v_abs = 0; // where to write the swapped entry

        ret = ktfs_map_block_and_or_allocate(mount, &superb, &root, v_lbn, &v_abs, 0);
        if(ret < 0) {
            lock_release(&mount->mount_lock);
            return ret;
        } // map directory block containing victim slot

        void* xblock = NULL;
        ret = cache_get_block(mount->cache, (unsigned long long)v_abs * KTFS_BLKSZ, &xblock);
        if(ret < 0){
            lock_release(&mount->mount_lock);
            return ret;
        } // pin target block in cache

        memcpy((uint8_t*)xblock + v_off, &last, entry_sz);
        cache_release_block(mount->cache, xblock, 1); // write last entry into victim slot and mark dirty
    } // swap delete to keep directory compact

    root.size -= entry_sz; // shrink directory by one entry

    ret = ktfs_write_to_ino(mount, superb.root_directory_inode, &superb, &root);
    if(ret < 0) {
        lock_release(&mount->mount_lock);
        return ret;
    } // commit updated directory inode

    lock_release(&mount->mount_lock); // release global mount lock
    return 0;
}


/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions
 * @details Any commands such as (FCNTL_GETEND, FCNTL_GETPOS, ...) should pass back through the arg
 * variable. Do not directly return the value.
 * @details FCNTL_GETEND should pass back the size of the file in bytes through the arg variable.
 * @details FCNTL_SETEND should set the size of the file to the value passed in through arg.
 * @details FCNTL_GETPOS should pass back the current position of the file pointer in bytes through
 * the arg variable.
 * @details FCNTL_SETPOS should set the current position of the file pointer to the value passed in
 * through arg.
 * @param uio the uio object of the file to perform the control function
 * @param cmd the operation to execute. KTFS should support FCNTL_GETEND, FCNTL_SETEND (CP2),
 * FCNTL_GETPOS, FCNTL_SETPOS.
 * @param arg the argument to pass in, may be different for different control functions
 * @return 0 if successful, negative error code if error
 */
int ktfs_cntl(struct uio* uio, int cmd, void* arg) {
    // FIXME
    if(!uio) return -EINVAL;// basic guard: must have a uio


    struct ktfs_uio *kuio = (struct ktfs_uio *)uio; // cast to our concrete type
    // struct ktfs_mount* mount = kuio->file.fs; // mount 
    // struct ktfs_file file = kuio->file; // snapshot of per-handle state
    // uint64_t size = file.size; // cached size (authoritative comes from inode elsewhere)
    // uint64_t offset = file.offset; // kept for spec completeness
    // uint64_t pos = file.position; // current file position

    if(cmd == FCNTL_GETEND){
        if(!arg) return -EINVAL; // need a place to put result
        lock_acquire(&kuio->file_lock); 
        *(unsigned long long*)arg = kuio->file.size; // report file length (bytes)
        lock_release(&kuio->file_lock);
        return 0;
    }
    else if(cmd==FCNTL_SETEND){
        if(!arg) return -EINVAL; // need target size

        // struct ktfs_uio* kuio  = (struct ktfs_uio*)uio;
        struct ktfs_mount* mount = kuio->file.fs; // mount for this file

        lock_acquire(&mount->mount_lock); // serialize size changes
        lock_acquire(&kuio->file_lock); // protect handle state
        
        unsigned long long newend = *(unsigned long long*)arg; // desired EOF
        // int ret;

        struct ktfs_superblock superb; // on-disk layout info

        struct ktfs_inode inode; // working inode copy

        int ret = ktfs_read_super(mount, &superb); // fetch superblock
        if(ret < 0) {
            lock_release(&kuio->file_lock);
            lock_release(&mount->mount_lock);
            return ret;
        }

        ret = ktfs_inode_grab(mount, kuio->inode_number, &superb, &inode); // load inode
        if(ret < 0){
            lock_release(&kuio->file_lock);
            lock_release(&mount->mount_lock);
            return ret;
        }

        if(newend > KTFS_MAX_FILE_SIZE) { // clamp to filesystem cap
            lock_release(&kuio->file_lock);
            lock_release(&mount->mount_lock);
            return -EINVAL;
        }

        uint64_t old_size = inode.size; // current logical size

        if(newend < old_size){ // shrinking not allowed in CP2
            lock_release(&kuio->file_lock);
            lock_release(&mount->mount_lock);
            return -EINVAL;
        }

        if(newend == old_size){ // no-op: just sync cache
            kuio->file.size = inode.size;
            if(kuio->file.position > kuio->file.size){
                kuio->file.position = kuio->file.size;
            }
            lock_release(&kuio->file_lock);
            lock_release(&mount->mount_lock);
            return 0;
        }

        uint64_t startingBlock = (old_size == 0) ? 0 : ((old_size + KTFS_BLKSZ - 1) / KTFS_BLKSZ); // first LBN to ensure
        uint64_t endingBlocks = (newend + KTFS_BLKSZ - 1) / KTFS_BLKSZ; // one past last LBN

        for(uint32_t x = (uint32_t)startingBlock; x < (uint32_t)endingBlocks; ++x) { // allocate/map blocks up to new EOF
            uint32_t absblk = 0;
            ret = ktfs_map_block_and_or_allocate(mount, &superb, &inode, x, &absblk, 1);
            if(ret < 0) {
                lock_release(&kuio->file_lock);
                lock_release(&mount->mount_lock);
                return ret;
            }
        }

        inode.size = (uint32_t)newend; // commit new logical size
        ret = ktfs_write_to_ino(mount, kuio->inode_number, &superb, &inode); // persist inode
        if(ret < 0) {
            lock_release(&kuio->file_lock);
            lock_release(&mount->mount_lock);
            return ret;
        }

        kuio->file.size = inode.size; // refresh uio metadata
        if(kuio->file.position > kuio->file.size) { // clamp file cursor
            kuio->file.position = kuio->file.size;
        }

        lock_release(&kuio->file_lock);
        lock_release(&mount->mount_lock);
        return 0;
    }

    else if(cmd == FCNTL_GETPOS){
        if(!arg) return -EINVAL; // need output pointer
        lock_acquire(&kuio->file_lock);
        *(unsigned long long*)arg = kuio->file.position; // expose current position
        lock_release(&kuio->file_lock);
        return 0;
    }


    else if(cmd==FCNTL_SETPOS){
        if(!arg) return -EINVAL; // must supply a new position
        lock_acquire(&kuio->file_lock);
        unsigned long long newpos = *(const unsigned long long *)arg;  // desired offset
            if(newpos > KTFS_MAX_FILE_SIZE) {
            lock_release(&kuio->file_lock);
            return -EINVAL;
        }

        kuio->file.position = (uint64_t)newpos;

        lock_release(&kuio->file_lock);
        return 0; 
    }

    return -ENOTSUP; // unknown/unsupported control op
}

/**
 * @brief Flushes the cache to the backing device
 * @return 0 if flush successful, negative error code if error
 */
void ktfs_flush(struct filesystem* fs) {
    // FIXME
    // struct ktfs_mount {
    // struct filesystem fs;
    // struct cache* cache;
    // };
    struct ktfs_mount *mount = (struct ktfs_mount*)fs; // downcast generic fs to our ktfs mount
    if(!mount || !mount->cache) return; // nothing to flush or invalid handle, bail fast
    cache_flush(mount->cache); // push any dirty cache blocks to backing storage

    
    
}

/**
 * @brief Closes the listing device represented by the uio pointer
 * @param uio The uio pointer of ls
 * @return None
 */
void ktfs_listing_close(struct uio* uio) {
    // FIXME
    struct ktfs_listing_uio *lsuio = (struct ktfs_listing_uio *)uio; // cast to listing type
    if(!lsuio){ 
        return; // nothing to free
    }
    kfree(lsuio); // free the listing struct
}

/**
 * @brief Reads all of the files names in the file system using ls and copies them into the
 * providied buffer
 * @param uio The uio pointer of ls
 * @param buf The buffer to copy the file names to
 * @param bufsz The size of the buffer
 * @return The size written to the buffer
 */
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz) {
    // FIXME
    struct ktfs_listing_uio *ls = (struct ktfs_listing_uio *)uio; // cast to listing uio

    uint32_t entry_sz = sizeof(struct ktfs_dir_entry); // size of one dir entry
    long result = 0; // size written to the buffer

    if(bufsz == 0){
        return 0; // nothing to read into
    }

    if((ls->root.size % entry_sz) != 0){
        return -EIO; // corrupt directory size
    }

    if(ls->next_index >= ls->total_entries){
        return 0; // no more entries
    }

    while(ls->next_index < ls->total_entries && result == 0){
        uint32_t idx = ls->next_index; // current index to try

        struct ktfs_dir_entry dent; // temp dir entry
        int ret = ktfs_dir_get_entry(ls->mount, &ls->super, &ls->root, idx, &dent); // fetch entry

        ls->next_index = idx + 1; // advance cursor for next call

        if(ret == -ENOENT){
            continue; // skip empty slot
        }

        if(ret < 0){
            return ret; // propagate error
        }

        dent.name[KTFS_MAX_FILENAME_LEN] = '\0'; // ensure null-terminated name
        size_t len = strlen(dent.name); // length of file name
        strncpy(buf, dent.name, bufsz); // copy name into user buffer
        result = (long)((len < bufsz) ? len : bufsz); // return bytes copied
    }

    return result; // no more names
}


static int ktfs_read_super(struct ktfs_mount* mount, struct ktfs_superblock* super){

    if(!mount || !super) return -EINVAL; // must have valid pointers for mount and superblock
    void *blk; // temp pointer for block data
    int ret = cache_get_block(mount->cache, 0, &blk); // read block 0 (superblock) from cache
    if(ret<0) return ret; // return if unable to fetch
    *super = *(struct ktfs_superblock*)blk; // copy superblock contents into provided struct
    cache_release_block(mount->cache, blk, 0); // release cache block (clean)
    return 0; // success
}


static void ktfs_compute_layout(const struct ktfs_superblock* sb, uint32_t* inode_bitmap_start, uint32_t* block_bitmap_start, uint32_t* inode_start,
                                uint32_t* data_start){
    // Compute offsets for all major filesystem regions
    int inode_bitmap_start_block = 1; // superblock = block 0
    int block_bitmap_start_block = inode_bitmap_start_block + sb->inode_bitmap_block_count;
    int inode_table_start_block = block_bitmap_start_block + sb->bitmap_block_count;
    int data_block_start_block = inode_table_start_block + sb->inode_block_count;

    // Write to outputs if requested
    if(inode_bitmap_start) *inode_bitmap_start = inode_bitmap_start_block;
    if(block_bitmap_start) *block_bitmap_start = block_bitmap_start_block;
    if(inode_start) *inode_start = inode_table_start_block;
    if(data_start)  *data_start = data_block_start_block;
}


static int ktfs_map_block(struct ktfs_mount* m, const struct ktfs_superblock* sb,
                              const struct ktfs_inode* ino, uint32_t lbn, uint32_t* absblk){
    if(!m || !sb || !ino || !absblk) return -EINVAL; //null checks

    uint32_t inode_start = 0, data_start = 0, tmp1 = 0;
    ktfs_compute_layout(sb, &tmp1, &tmp1, &inode_start, &data_start); // compute where inode and data regions start

    const uint32_t indirect_index = (KTFS_BLKSZ / sizeof(uint32_t)); // number of block entries in one indirect block
    //direct
    if(lbn<KTFS_NUM_DIRECT_DATA_BLOCKS){
        uint32_t idx = ino->block[lbn]; // grab block index from direct list
        // if(idx == 0){
        //     return -ENOENT; // hole (unallocated block)
        // }
        *absblk = data_start + idx; // translate logical index to absolute block number
        return 0;
    }

    lbn -= KTFS_NUM_DIRECT_DATA_BLOCKS; // move past direct blocks

    //indirect case
    if(lbn<indirect_index){
        // if(ino->indirect == 0) return -ENOENT; // no indirect block present
        void* p = NULL;
        int return_code = cache_get_block(m->cache, (unsigned long long)(data_start + ino->indirect) * KTFS_BLKSZ, &p); // fetch indirect block
        if(return_code < 0) return return_code;
        uint32_t *entries = (uint32_t *)p;

        
        // lbn tells us which logical file block we want
        uint32_t idx = entries[lbn]; // lookup block index
        cache_release_block(m->cache, p, 0); // release cache reference
        // if(idx == 0){ 
        //     return -ENOENT; // sparse entry
        // }
        *absblk = data_start + idx; // compute absolute block
        return 0;
    
    }

    uint32_t width = indirect_index * indirect_index; // how many blocks one double indirect covers

    lbn-=indirect_index; // skip past indirect range
    
    for(int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS; i++){
        if(lbn<width){
            // if(ino->dindirect[i] == 0) return -ENOENT; // no double indirect allocated
            void* block;
            int return_code = cache_get_block(m->cache, (unsigned long long)(data_start + ino->dindirect[i]) * KTFS_BLKSZ, &block); // fetch dindirect table
            if(return_code < 0) return return_code;

            uint32_t index1 = lbn/indirect_index; // which indirect inside this dindirect
            uint32_t index2 = lbn%indirect_index; // which entry inside that indirect
            uint32_t *x = (uint32_t*)block;

            uint32_t indirect_idx = x[index1]; // get pointer to indirect block
            cache_release_block(m->cache, block, 0);

            // if(indirect_idx == 0) return -ENOENT; // hole in first-level table

            void* block2 = NULL;
            return_code = cache_get_block(m->cache, (unsigned long long)(data_start + indirect_idx) * KTFS_BLKSZ, &block2); // fetch second-level indirect
            if(return_code < 0) return return_code;

            uint32_t *y = (uint32_t *)block2;

            uint32_t idx = y[index2]; // get actual data block index
            cache_release_block(m->cache, block2, 0);

            // if(idx == 0) return -ENOENT; // hole (no data block)
            *absblk = data_start+idx; // map to absolute block
            return 0;
        }  

        lbn -= width; // move to next double indirect section
    }
    return -ENOENT; // block not found in mapping


}



static int ktfs_dir_get_entry(struct ktfs_mount* m, const struct ktfs_superblock* sb, const struct ktfs_inode* dir_ino, uint32_t idx,
                              struct ktfs_dir_entry* out){

    if(!m || !sb || !dir_ino || !out){
        return -EINVAL; // null checks
    }
    uint32_t entry_size = sizeof(struct ktfs_dir_entry); //fixed size of each directory entry

    if(dir_ino->size % entry_size != 0) return -EIO; //directory size should be a multiple of entry size

    uint32_t nents = dir_ino->size / entry_size; //number of entries in the dir
    if(idx >= nents) return -ENOENT; //out of bounds index


    int index_byte_offset = entry_size*idx; // byte offset of requested entry
    uint32_t lbn = index_byte_offset/KTFS_BLKSZ; //which logical block contains it
    uint32_t off_in_block = index_byte_offset% KTFS_BLKSZ; //byte offset within that block

    uint32_t absblk = 0; //will hold absolute block number

    int return_code = ktfs_map_block(m, sb, dir_ino, lbn, &absblk);//translate lbn to absolute block

    if(return_code<0) return return_code; //propagate mapping or hole error

    void* blk=NULL; // pointer for cached block

    return_code = cache_get_block(m->cache, (unsigned long long)(absblk) * KTFS_BLKSZ, &blk); // load block into cache
    if(return_code<0){
        return return_code; // device/cache error
    }


    struct ktfs_dir_entry* entry = (struct ktfs_dir_entry*)((uint8_t*)blk + off_in_block); // locate entry within block
    *out = *entry; // copy out the entry

    out->name[KTFS_MAX_FILENAME_LEN] = '\0'; // ensure null-termination (defensive)

    cache_release_block(m->cache, blk, 0); // release without marking dirty

    return 0; // success

}


static int ktfs_inode_grab(struct ktfs_mount* mount, uint32_t inode_num,  const struct ktfs_superblock* super, struct ktfs_inode* dest){
    if(!mount || !super || !dest) return -EINVAL; //null check

    uint32_t itbl_start = 0; 
    uint32_t data_ignored = 0;
    uint32_t scratch = 0; // placeholders for layout computation
    ktfs_compute_layout(super, &scratch, &scratch, &itbl_start, &data_ignored); // figure out where inode table starts

    uint64_t ino_off = (uint64_t)inode_num * sizeof(struct ktfs_inode); // byte offset of desired inode in inode table
    uint64_t blk_idx =ino_off / KTFS_BLKSZ; // which block inside inode table
    uint64_t blk_byte_off = (uint64_t)itbl_start * KTFS_BLKSZ + blk_idx * KTFS_BLKSZ; // byte offset on disk for that block
    size_t within = (size_t)(ino_off % KTFS_BLKSZ); // offset inside the block where inode begins

    void* p = NULL; // pointer to cached block memory
    int rc = cache_get_block(mount->cache, blk_byte_off, &p); // fetch the block containing the inode
    if(rc < 0) return rc; // return immediately if cache read failed

    memcpy(dest, (uint8_t*)p + within, sizeof(*dest)); // copy inode contents into destination
    cache_release_block(mount->cache, p, 0); // release cache block (not dirty)
    return 0; // success
}


static int ktfs_bitmap_indices_fetch(const struct ktfs_superblock* sb, int bitmap_id, uint32_t* blk_start_out, uint32_t* blk_cnt_out, uint32_t* total_bits_out){
    if(!sb || !blk_start_out || !blk_cnt_out || !total_bits_out) { 
        return -EINVAL; 
    }

    uint32_t inode_bitmap_start_index = 0; // start block of inode bitmap
    uint32_t data_bitmap_start_index = 0; // start block of data bitmap
    uint32_t inode_region_ignored = 0; // unused out for layout
    uint32_t data_region_ignored = 0; // unused out for layout
    ktfs_compute_layout(sb, &inode_bitmap_start_index, &data_bitmap_start_index, &inode_region_ignored, &data_region_ignored); // compute layout anchors

    switch(bitmap_id) {
        case 0:{ // inode bitmap
            const uint32_t inodes_per_block = KTFS_BLKSZ / sizeof(struct ktfs_inode); // inodes per table block
            *blk_start_out= inode_bitmap_start_index; // first bitmap block
            *blk_cnt_out = sb->inode_bitmap_block_count; // number of bitmap blocks
            *total_bits_out = sb->inode_block_count * inodes_per_block; // total addressable inodes
            return 0; // done
        }
        default:{ // data bitmap
            *blk_start_out = data_bitmap_start_index; // first data-bitmap block
            *blk_cnt_out = sb->bitmap_block_count; // number of data-bitmap blocks
            *total_bits_out = sb->block_count; // total addressable data blocks
            return 0; // done
        }
    }
}


//done
static int ktfs_bitmap_free_bit_detect(struct ktfs_mount* m, const struct ktfs_superblock* sb, int kind, uint32_t* outIndex){
    if(!m || !sb || !outIndex) return -EINVAL; // basic arg validation

    uint32_t bitmap_start_index = 0; // bitmap start block
    uint32_t bitmap_blocks = 0; // number of bitmap blocks
    uint32_t total_bits = 0; // total addressable bits
    int ret = ktfs_bitmap_indices_fetch(sb, kind, &bitmap_start_index, &bitmap_blocks, &total_bits); // query bounds
    if(ret < 0) return ret; // propagate error


    uint32_t ibitmap = 0;
    uint32_t dbitmap = 0; 
    uint32_t itbl = 0;
    uint32_t data0 = 0; // layout anchors
    ktfs_compute_layout(sb, &ibitmap, &dbitmap, &itbl, &data0); // compute first data block index

    uint32_t first_allowed = 0; // first bit we may consider
    if(kind != 0) {
        first_allowed = data0; // skip metadata for data bitmap
        if(first_allowed >= total_bits) return -EINVAL; // out of range
    }

    const uint32_t max_bits_per_bitmap = (uint32_t)KTFS_BLKSZ * 8u; // bits per bitmap block

    uint32_t start_bit = first_allowed; // starting search bit
    uint32_t blk0 = start_bit / max_bits_per_bitmap; // initial bitmap block
    if(blk0 >= bitmap_blocks) {
        return -ENOENT; // beyond bitmap
    }
    uint32_t bit_in_blk0  = start_bit % max_bits_per_bitmap; // bit offset inside first block
    uint32_t byte0 = bit_in_blk0 / 8u; // starting byte in first block
    uint32_t bit_in_byte0 = bit_in_blk0 % 8u; // starting bit within that byte

    for(uint32_t b = blk0; b < bitmap_blocks; ++b) { // walk bitmap blocks
        void* raw = NULL;
        ret = cache_get_block(m->cache, (unsigned long long)(bitmap_start_index + b) * KTFS_BLKSZ, &raw); // map bitmap block
        if(ret < 0) return ret;

        uint8_t* bytes = (uint8_t*)raw; // byte view
        uint32_t base_bit = b * max_bits_per_bitmap; // bit index of first bit in this block

        uint32_t first_byte = (b == blk0) ? byte0 : 0u; // first byte to examine

        for(uint32_t by = first_byte; by < KTFS_BLKSZ; ++by) { // scan bytes
            uint32_t byte_base_bit = base_bit + by * 8u; // bit index of first bit in this byte

            if(byte_base_bit >= total_bits) { // reached logical end
                cache_release_block(m->cache, raw, 0);
                return -ENOENT;
            }

            uint8_t window_mask = 0xFFu; // candidate bits in this byte

            if(b == blk0 && by == first_byte) { // mask off leading bits before start
                window_mask = (uint8_t)(0xFFu & (~((1u << bit_in_byte0) - 1u)));
            }

            uint32_t remaining_bits = total_bits - byte_base_bit; // bits left overall
            if(remaining_bits < 8u) { // tail short byte
                uint8_t tail = (remaining_bits == 8u) ? 0xFFu : (uint8_t)((1u << remaining_bits) - 1u);
                window_mask &= tail; // keep only valid tail bits
            }

            if(window_mask == 0) continue; // nothing to check

            uint8_t cur = bytes[by]; // bitmap byte

            for(uint32_t bitp = 0; bitp < 8u; ++bitp) { // test each bit
                uint8_t bit_sel = (uint8_t)(1u << bitp); // selector
                if((window_mask & bit_sel) == 0) continue; // masked out
                if((cur & bit_sel) != 0) continue; // already set

                uint32_t found = byte_base_bit + bitp; // free bit index
                if(found < first_allowed) 
                    continue; // below allowed range
                if(found >= total_bits) { // safety bound
                    cache_release_block(m->cache, raw, 0);
                    return -ENOENT;
                }

                *outIndex = found; // report free bit
                cache_release_block(m->cache, raw, 0);
                return 0; // success
            }
        }

        cache_release_block(m->cache, raw, 0); // advance to next block
    }

    return -ENOENT; // no free bit found
}



static int ktfs_bitmap_mark(struct ktfs_mount* m, const struct ktfs_superblock* superb, int kind, uint32_t index){
    if(!m || !superb) {
        return -EINVAL;
    }

    uint32_t bitmap_start_index = 0; // bitmap start block
    uint32_t bitmap_count = 0; // number of bitmap blocks
    uint32_t max_bits = 0; // total addressable bits
    int ret = ktfs_bitmap_indices_fetch(superb, kind, &bitmap_start_index, &bitmap_count, &max_bits); // query bitmap bounds
    if(ret < 0) {
        return ret;
    }

    if(index >= max_bits) { // index out of range
        return -EINVAL;
    }

    const uint32_t bits_per_bitmap = KTFS_BLKSZ * 8; // bits per bitmap block
    const uint32_t blk_idx= index / bits_per_bitmap; // bitmap block index
    const uint32_t bit_off= index % bits_per_bitmap; // bit offset within block
    const uint32_t byte_off =bit_off / 8; // target byte in bitmap block
    const uint32_t bit_in_byte = bit_off % 8; // target bit inside byte

    if(blk_idx >= bitmap_count || byte_off >= KTFS_BLKSZ) { // bounds check within bitmap
        return -EINVAL;
    }

    void* blk_ptr = NULL; // mapped bitmap block
    uint64_t dev_off = (uint64_t)(bitmap_start_index + blk_idx) * KTFS_BLKSZ; // device offset for bitmap block
    ret = cache_get_block(m->cache, dev_off, &blk_ptr); // read bitmap block into cache
    if(ret < 0) {
        return ret;
    }

    ((uint8_t*)blk_ptr)[byte_off] |= (uint8_t)(1u << bit_in_byte); // set allocation bit

    cache_release_block(m->cache, blk_ptr, 1); // release and mark dirty
    return 0;
}

static int ktfs_bitmap_unmark(struct ktfs_mount* m, const struct ktfs_superblock* superb, int kind, uint32_t index){
    int ret = 0; // return code accumulator
    void* p = NULL; // mapped bitmap block pointer
    int dirty = 0; // whether to mark cache line dirty on release

    if(!m || !superb){
        return -EINVAL;
    } 
    else{
        uint32_t start_block = 0; // first bitmap block
        uint32_t numberBlocks = 0; // number of bitmap blocks
        uint32_t max_bits = 0; // total addressable bits

        ret = ktfs_bitmap_indices_fetch(superb, kind, &start_block, &numberBlocks, &max_bits); // query bounds
        if(ret >= 0){
            const uint32_t bits_per_bitmap = KTFS_BLKSZ * 8; // bits per bitmap block
            uint32_t blk_idx = index / bits_per_bitmap; // bitmap block index
            uint32_t within_b= index % bits_per_bitmap; // bit offset within block
            uint32_t byte_off = within_b / 8; // target byte in block
            uint32_t bit = within_b % 8; // target bit within byte

            if(index >= max_bits || blk_idx >= numberBlocks || byte_off >= KTFS_BLKSZ){
                return -EINVAL;
            } 
            else{
                uint64_t off = (unsigned long long)(start_block + blk_idx) * KTFS_BLKSZ; // device offset of bitmap block
                ret = cache_get_block(m->cache, off, &p); // read bitmap block
                if(ret >= 0){
                    uint8_t* bytes = (uint8_t*)p; // byte view
                    uint8_t mask = (uint8_t)(1u << bit); // bit selector
                    bytes[byte_off] &= (uint8_t)~mask; // clear allocation bit
                    dirty = 1; // mark dirty to persist
                }
            }
        }
    }

    if(p){
        cache_release_block(m->cache, p, dirty); // release bitmap block
    }
    return ret; // success or error code
}

static int ktfs_write_to_ino(struct ktfs_mount* mount, uint32_t inode_num, const struct ktfs_superblock* superb, const struct ktfs_inode* src){
    
    if(mount == NULL || superb == NULL || src == NULL) {
        return -EINVAL;
    }

    uint32_t itbl_start = 0;
    uint32_t data_ignored = 0;
    uint32_t scratch = 0;
    uint64_t ino_off;
    
    uint64_t blk_idx;
    uint64_t blk_byte_off;

    size_t within;
    void* p = NULL;
    int ret;

    ktfs_compute_layout(superb, &scratch, &scratch, &itbl_start, &data_ignored);
    ino_off = (uint64_t)inode_num * sizeof(struct ktfs_inode);
    blk_idx = ino_off / KTFS_BLKSZ;
    within = (size_t)(ino_off % KTFS_BLKSZ);
    blk_byte_off = (uint64_t)itbl_start * KTFS_BLKSZ + blk_idx * KTFS_BLKSZ;

    ret = cache_get_block(mount->cache, blk_byte_off, &p);
    if(ret < 0) {
        return ret;
    }

    memcpy((uint8_t*)p + within, src, sizeof(*src));
    cache_release_block(mount->cache, p, 1);

    return 0;
}

//done
static int ktfs_inode_free_all_blocks(struct ktfs_mount* mount, const struct ktfs_superblock* superb, struct ktfs_inode* ino){
    if(!mount || !superb || !ino) {
        return -EINVAL; 
    }

    uint32_t inode_start = 0; 
    uint32_t data_start = 0; // data region start block
    uint32_t scratch = 0; 

    ktfs_compute_layout(superb, &scratch, &scratch, &inode_start, &data_start); // compute region offsets

    int ret = 0; // sticky error
    uint32_t entries_per_block = KTFS_BLKSZ / sizeof(uint32_t); // pointers per indirection block
    uint32_t total_blocks = 0; // blocks covering file size
    if(ino->size > 0) {
        total_blocks = (uint32_t)((ino->size + KTFS_BLKSZ - 1) / KTFS_BLKSZ); // ceil(size/BLKSZ)
    }

    for(int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; ++i) { // free direct blocks
        if((uint32_t)i < total_blocks) {
            uint32_t blk = ino->block[i]; // data index
            uint32_t phys_block = data_start + blk; // absolute block
            ret = ktfs_bitmap_unmark(mount, superb, 1, phys_block); // clear data bit
            if(ret < 0) {
                return ret; // propagate error
            }
            ino->block[i] = 0; // clear pointer
        } 
        else{
            ino->block[i] = 0; // clear unused slot
        }
    }

    uint32_t direct_limit = KTFS_NUM_DIRECT_DATA_BLOCKS; // first LBN after directs
    uint32_t used_indirect_blocks = 0; // how many data blocks live under single indirect
    if(total_blocks > direct_limit) {
        used_indirect_blocks = total_blocks - direct_limit; // blocks past direct
        if(used_indirect_blocks > entries_per_block) {
            used_indirect_blocks = entries_per_block; // clamp to table size
        }
    }

    if(used_indirect_blocks == 0){
        ino->indirect = 0; // no single-indirect in use
    } 
    else{
        uint32_t indirect_block = data_start + ino->indirect; // abs L1 table block
        void* block = NULL; // cache ptr

        int rc = cache_get_block(mount->cache, (unsigned long long)(indirect_block * KTFS_BLKSZ), &block); // map L1
        if(rc < 0) {
            return rc; // I/O error
        }

        uint32_t* ptr = (uint32_t*)block; // L1 entries

        for(uint32_t n = 0; n < entries_per_block; ++n) { // walk all L1 entries
            if(n < used_indirect_blocks) {
                uint32_t b = ptr[n]; // data idx
                uint32_t phys = data_start + b; // abs data block
                int e = ktfs_bitmap_unmark(mount, superb, 1, phys); // free data block
                if((e < 0) && (ret == 0)) {
                    ret = e; // keep first error
                    return ret;
                }
                ptr[n] = 0; // clear entry
            } 
            else{
                ptr[n] = 0; // zero unused entry
            }
        }

        cache_release_block(mount->cache, block, 1); // write back cleared L1
        if(ret < 0) {
            return ret; // bail if data free failed
        }

        rc = ktfs_bitmap_unmark(mount, superb, 1, indirect_block); // free L1 block
        if(rc < 0) {
            return rc;
        }

        ino->indirect = 0; // clear pointer
    }

    uint32_t dind_start_lbn = direct_limit + entries_per_block; // first LBN in dind
    uint32_t remaining_blocks = 0; // blocks remaining to cover under dind
    if(total_blocks > dind_start_lbn) {
        remaining_blocks = total_blocks - dind_start_lbn; // number assigned under dind
    }

    uint32_t width = entries_per_block * entries_per_block; // blocks per dind table

    for(int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS; ++i) { // for each dind table
        if(remaining_blocks == 0) {
            ino->dindirect[i] = 0; // nothing used here
            continue;
        }

        uint32_t blocks_here = (remaining_blocks < width) ? remaining_blocks : width; // covered by this dind

        uint32_t double_indirect_index_num = ino->dindirect[i]; // L1 table idx
        uint32_t double_ind_block_no = data_start + double_indirect_index_num; // abs L1 block
        void* dblock = NULL; // cache ptr

        int rc = cache_get_block(mount->cache, (unsigned long long)(double_ind_block_no * KTFS_BLKSZ), &dblock); // map L1
        if(rc < 0){
            return rc;
        }

        uint32_t* first_level = (uint32_t*)dblock; // L1 entries
        uint32_t remaining_in_this = blocks_here; // yet to free under this dind

        for(int j = 0; j < (int)entries_per_block; ++j) { // walk L1 entries
            if(remaining_in_this == 0) {
                first_level[j] = 0; // zero unused L1 slot
                continue;
            }

            uint32_t under_this = (remaining_in_this < entries_per_block) ? remaining_in_this : entries_per_block; // L2 span

            uint32_t indirect_number = first_level[j]; // L2 table idx
            uint32_t ind_block = data_start + indirect_number; // abs L2 block
            void* iblock = NULL; // cache ptr

            int e = cache_get_block(mount->cache, (unsigned long long)(ind_block * KTFS_BLKSZ), &iblock); // map L2
            if(e < 0) {
                cache_release_block(mount->cache, dblock, 0); // drop L1 cleanly
                return e;
            }

            uint32_t* level2 = (uint32_t*)iblock; // L2 entries

            for(int k = 0; k < (int)entries_per_block; ++k) { // walk L2 entries
                if((uint32_t)k < under_this) {
                    uint32_t data_idx = level2[k]; // data idx
                    uint32_t phys_block = data_start + data_idx; // abs data block
                    int e2 = ktfs_bitmap_unmark(mount, superb, 1, phys_block); // free data block
                    if(e2 < 0 && ret == 0) {
                        ret = e2; // keep first error
                    }
                    level2[k] = 0; // clear entry
                } 
                else{
                    level2[k] = 0; // zero unused slot
                }
            }

            cache_release_block(mount->cache, iblock, 1); // write back cleared L2

            int e3 = ktfs_bitmap_unmark(mount, superb, 1, ind_block); // free L2 block
            if(e3 < 0 && ret == 0) {
                ret = e3;
            }

            first_level[j] = 0; // clear L1 entry
            remaining_in_this -= under_this; // advance remaining
        }

        cache_release_block(mount->cache, dblock, 1); // write back cleared L1
        if(ret < 0) {
            return ret; // bail on first recorded error
        }

        int e4 = ktfs_bitmap_unmark(mount, superb, 1, double_ind_block_no); // free L1 (dind) block
        if(e4 < 0) return e4;

        ino->dindirect[i] = 0; // clear dind pointer
        remaining_blocks -= blocks_here; 
    }

    ino->size = 0; // file now empty
    return 0; // success
}




static int ktfs_map_block_and_or_allocate(struct ktfs_mount* m, const struct ktfs_superblock* sb,struct ktfs_inode* ino, uint32_t lbn, uint32_t* absblk, int allocE){
    if(!m || !sb || !ino || !absblk) return -EINVAL; 

    // DEBUG: GK
    // If caller requested no allocation, just behave like ktfs_map_block
    if(!allocE){

        return ktfs_map_block(m, sb, ino, lbn, absblk);
    }

    uint32_t data_start = 0;
    uint32_t scratch = 0;
    ktfs_compute_layout(sb, &scratch, &scratch, &scratch, &data_start); // get data region layout

    const uint32_t ents_per = KTFS_BLKSZ / sizeof(uint32_t); // entries per indirect table

    //direct
    if(lbn < KTFS_NUM_DIRECT_DATA_BLOCKS) {
        uint32_t idx = ino->block[lbn]; // data index from direct slot
        if(idx == 0) {
            uint32_t calc_abs_no = 0;
            int rc = ktfs_alloc_zero_block(m, sb, &calc_abs_no); // allocate zeroed block
            if(rc < 0) return rc;
            ino->block[lbn] = calc_abs_no - data_start; // store relative index
            *absblk = calc_abs_no; // return absolute block
            return 0;
        }
        *absblk = data_start + idx; // translate to absolute block
        return 0;
    }

    //indirect
    lbn -= KTFS_NUM_DIRECT_DATA_BLOCKS; // move into indirect range
    if(lbn < ents_per) {
        if(ino->indirect == 0) {
            uint32_t calc_abs_no = 0;
            int rc = ktfs_alloc_zero_block(m, sb, &calc_abs_no); // alloc L1 table
            if(rc < 0) return rc;
            ino->indirect = calc_abs_no - data_start; // store table index
        }

        uint32_t indirect_abs_block_no = data_start + ino->indirect; // absolute L1 table block
        void* p = NULL;
        int rc = cache_get_block(m->cache, (unsigned long long)indirect_abs_block_no * KTFS_BLKSZ, &p); // map L1
        if(rc < 0) return rc;
        uint32_t* tbl = (uint32_t*)p; // L1 entries
        uint32_t idx = tbl[lbn]; // entry for this lbn

        if(idx == 0) {
            uint32_t calc_abs_no = 0;
            rc = ktfs_alloc_zero_block(m, sb, &calc_abs_no); // alloc data block
            if(rc < 0) { 
                cache_release_block(m->cache, p, 0); 
                return rc; 
            }
            tbl[lbn] = calc_abs_no - data_start; // set relative index
            cache_release_block(m->cache, p, 1); // dirty: updated table
            *absblk = calc_abs_no; // return absolute
            return 0;
        } 
        else{
            cache_release_block(m->cache, p, 0); // clean
            *absblk = data_start + idx; // return absolute
            return 0;
        }
    }

    //dindirect
    lbn -= ents_per; // move into double-indirect range
    const uint32_t width = ents_per * ents_per; // coverage per dind block

    for(int di = 0; di < KTFS_NUM_DINDIRECT_BLOCKS; ++di) {
        if(lbn >= width) { lbn -= width; continue; } // skip to the correct dind slot

        if(ino->dindirect[di] == 0) {
            uint32_t calc_abs_no = 0, 
            rc = ktfs_alloc_zero_block(m, sb, &calc_abs_no); // allocate L1 table
            if(rc < 0) return rc;
            ino->dindirect[di] = calc_abs_no - data_start; // store relative index
        }

        uint32_t i1 = lbn / ents_per, i2 = lbn % ents_per; 
        uint32_t double_indirect_absolute_block = data_start + ino->dindirect[di]; 
        void* p1 = NULL;
        int rc = cache_get_block(m->cache, (unsigned long long)double_indirect_absolute_block * KTFS_BLKSZ, &p1); // map L1
        if(rc < 0) return rc;
        uint32_t* lvl1 = (uint32_t*)p1;
        uint32_t indirect_number = lvl1[i1]; // L2 table index

        if(indirect_number == 0) {
            uint32_t calc_abs_no = 0;
            rc = ktfs_alloc_zero_block(m, sb, &calc_abs_no); // allocate L2 table
            if(rc < 0) { 
                cache_release_block(m->cache, p1, 0); 
                return rc; 
            }
            indirect_number = calc_abs_no - data_start;
            lvl1[i1] = indirect_number; // link L2
            cache_release_block(m->cache, p1, 1); // dirty: updated L1
        } 
        else{
            cache_release_block(m->cache, p1, 0); // clean
        }

        uint32_t indirect_abs_block_no = data_start + indirect_number; // absolute L2 block
        void* p2 = NULL;
        rc = cache_get_block(m->cache, (unsigned long long)indirect_abs_block_no * KTFS_BLKSZ, &p2); // map L2
        if(rc < 0) return rc;
        uint32_t* lvl2 = (uint32_t*)p2;
        uint32_t data_idx = lvl2[i2]; // data index

        if(data_idx == 0){
            uint32_t calc_abs_no = 0;
            rc = ktfs_alloc_zero_block(m, sb, &calc_abs_no); // allocate data block
            if(rc < 0) { 
                cache_release_block(m->cache, p2, 0); 
                return rc; 
            }
            lvl2[i2] = calc_abs_no - data_start; // store relative index
            cache_release_block(m->cache, p2, 1); // dirty: updated L2
            *absblk = calc_abs_no; // return absolute
            return 0;
        } 
        else{
            cache_release_block(m->cache, p2, 0); // clean
            *absblk = data_start + data_idx; // return absolute
            return 0;
        }
    }

    return -ENOENT; 
}


static int ktfs_alloc_zero_block(struct ktfs_mount* m,const struct ktfs_superblock* sb, uint32_t* out_abs){
    if(!m || !sb || !out_abs) return -EINVAL; 
    uint32_t calc_abs_no = 0; // candidate absolute block
    int ret = ktfs_bitmap_free_bit_detect(m, sb, 1, &calc_abs_no); // find a free data block bit
    if(ret < 0) return ret; // propagate error

    ret = ktfs_bitmap_mark(m, sb, 1, calc_abs_no); // mark block as allocated
    if(ret < 0) return ret; // propagate error

    void* block = NULL; // cache-mapped block
    cache_get_block(m->cache, (unsigned long long)calc_abs_no * KTFS_BLKSZ, &block); // map the block
    memset(block, 0, KTFS_BLKSZ); // zero-fill the new block
    cache_release_block(m->cache, block, 1); // release and mark dirty

    *out_abs = calc_abs_no; // return absolute block number
    return 0; // success
}