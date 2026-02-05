/*! @file memory.c
    @brief Physical and virtual memory manager
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

//#include <cstdint>
#include <stddef.h>
#include <stdint.h>
#ifdef MEMORY_TRACE
#define TRACE
#endif

#ifdef MEMORY_DEBUG
#define DEBUG
#endif

#include "memory.h"

#include "conf.h"
#include "console.h"
#include "error.h"
#include "heap.h"
#include "misc.h"
#include "process.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"

// COMPILE-TIME CONFIGURATION
//

// Minimum amount of memory in the initial heap block.

#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif

// INTERNAL CONSTANT DEFINITIONS
//

#define MEGA_SIZE ((1UL << 9) * PAGE_SIZE)  // megapage size
#define GIGA_SIZE ((1UL << 9) * MEGA_SIZE)  // gigapage size

#define PTE_ORDER 3
#define PTE_CNT (1U << (PAGE_ORDER - PTE_ORDER))

#ifndef PAGING_MODE
#define PAGING_MODE RISCV_SATP_MODE_Sv39
#endif

#ifndef ROOT_LEVEL
#define ROOT_LEVEL 2
#endif

// IMPORTED GLOBAL SYMBOLS
//

// linker-provided (kernel.ld)
extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];

// EXPORTED GLOBAL VARIABLES
//

char memory_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

// We keep free physical pages in a linked list of _chunks_, where each chunk
// consists of several consecutive pages of memory. Initially, all free pages
// are in a single large chunk. To allocate a block of pages, we break up the
// smallest chunk on the list.

/**
 * @brief Section of consecutive physical pages. We keep free physical pages in a
 * linked list of chunks. Initially, all free pages are in a single large chunk. To
 * allocate a block of pages, we break up the smallest chunk in the list
 */
struct page_chunk {
    struct page_chunk *next;  ///< Next page in list
    unsigned long pagecnt;    ///< Number of pages in chunk
};

/**
 * @brief RISC-V PTE. RTDC (RISC-V docs) for what each of these fields means!
 */
struct pte {
    uint64_t flags : 8;
    uint64_t rsw : 2;
    uint64_t ppn : 44;
    uint64_t reserved : 7;
    uint64_t pbmt : 2;
    uint64_t n : 1;
};

// INTERNAL MACRO DEFINITIONS
//

#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2 * 9)) % PTE_CNT)
#define VPN1(vma) ((VPN(vma) >> (1 * 9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0 * 9)) % PTE_CNT)

// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).

#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)

#define PT_INDEX(lvl, vpn) \
    (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) >> (lvl * (PAGE_ORDER - PTE_ORDER)))
// INTERNAL FUNCTION DECLARATIONS
//

static void ptab_reset(struct pte *ptab  // page table to reset
);

static struct pte *ptab_clone(struct pte *ptab  // page table to clone
);

static void ptab_discard(struct pte *ptab  // page table to discard
);

static void ptab_insert(struct pte *ptab,   // page table to modify
                        unsigned long vpn,  // virtual page number to insert
                        void *pp,           // pointer to physical page to insert
                        int rwxug_flags     // flags for inserted mapping
);

static void *ptab_remove(struct pte *ptab, unsigned long vpn);

static void ptab_adjust(struct pte *ptab, unsigned long vpn, int rwxug_flags);

struct pte *ptab_fetch(struct pte *ptab, unsigned long vpn);

static inline mtag_t active_space_mtag(void);
static inline mtag_t ptab_to_mtag(struct pte *root, unsigned int asid);
static inline struct pte *mtag_to_ptab(mtag_t mtag);
static inline struct pte *active_space_ptab(void);

static inline void *pageptr(uintptr_t n);
static inline uintptr_t pagenum(const void *p);
static inline int wellformed(uintptr_t vma);

static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag);
static inline struct pte null_pte(void);

// INTERNAL GLOBAL VARIABLES
//

static mtag_t main_mtag;

static struct pte main_pt2[PTE_CNT] __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt1_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct page_chunk *free_chunk_list;

// Global variable for the start of the free region in the chunk
//
static void * free_base_addr;

// EXPORTED FUNCTION DECLARATIONS
//

void memory_init(void) {

    // Linker defined symbols that are lables for addresses/boundaries for our kernel image
    // Made read-only by the const void *const, pointer value cannot change and you cant write through the pointer
    // Come from kernel.ld and will be used to compute the sizes and align the pages for when we initialie the free chunk space
    // In terms of Sv39, for my own understanding, this sets the PTE permissions
    const void *const text_start = _kimg_text_start;
    const void *const text_end = _kimg_text_end;
    const void *const rodata_start = _kimg_rodata_start;
    const void *const rodata_end = _kimg_rodata_end;
    const void *const data_start = _kimg_data_start;

    // Initial heap region that will be initialized
    void *heap_start;
    void *heap_end;

    uintptr_t pma;
    const void *pp;

    trace("%s()", __func__);

    // Check to make sure that the kernel is linekd to start exactly at RAM_START, otherwise fail
    assert(RAM_START == _kimg_start);

    debug("           RAM: [%p,%p): %zu MB", RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    debug("  Kernel image: [%p,%p)", _kimg_start, _kimg_end);

    // Kernel must fit inside 2MB megapage (one level 1 PTE)

    // Makes sure the kernel fits into a 2MB megapage, if not it panics
    if (MEGA_SIZE < _kimg_end - _kimg_start) panic(NULL);

    // Initialize main page table with the following direct mapping:
    //
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB

    // Identity mapping of MMIO region as two gigapage mappings
    // Need devices, UART PLIC VirtIO, to keep working after the paging starts
    // Loop through the devices in MMIO space and do idendity mapping
    // This allows for the devices to still be reachable after pahinh
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void *)pma, PTE_R | PTE_W | PTE_G);

    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.

    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages

    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Enable paging; this part always makes me nervous. 
    // Everything is identity mapped so it sho
    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = (void *)ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);

    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += ROUND_UP(HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end) panic("out of memory");

    // Initialize heap memory manager

    heap_init(heap_start, heap_end);

    debug("Heap allocator: [%p,%p): %zu KB free", heap_start, heap_end,
          (heap_end - heap_start) / 1024);

    // FIXME: Initialize the free chunk list here

    // Already built the page tables above and the heap is set up
    // Set up the unused RAM
    // Make sure the chunks are aligned and manageable and store in a linked list to be used later

    // Start of the free physical RAM after the heap ends
    // Macro ROUND_IP to align with the pagesize
    void * free_start = (void *) ROUND_UP((uintptr_t) heap_end, PAGE_SIZE);

    // The free unused RAM ends at the end of the RAM given to us already
    void * free_end = RAM_END;

    // Create the conditional statement that checks that there is free space to use
    if(free_end > free_start){

        // Compute how many pages fit in the free space
        // Compute byte count first
        unsigned long bytecnt = ((uintptr_t) free_end - (uintptr_t) free_start);

        // Divide the number of bytes by the page size
        // Do this right shifting by the page order which is 12 as there are 2^12 bytes in a page
        unsigned long pagecnt = bytecnt >> PAGE_ORDER;

        // Place the first chunk struct at free_start instead of on the heap
        struct page_chunk * chunk_node = (struct page_chunk *) free_start;

        // If properly initialized, record how many pages this chunk represents
        chunk_node->pagecnt = pagecnt;

        // This is the only chunk initially so set the next to null
        chunk_node->next = NULL;

        // Remeber the physical starting base address of thsi chunk so that can reference where this specific chunk starts
        free_base_addr = free_start;

        // Make this chunk nod the head of the free list as this is the only chunk so far
        free_chunk_list = chunk_node;
        
    }

    // Condition where there is no free space and free_start > free_end
    else{

        // Start with an empty list
        free_chunk_list = NULL;
    }

    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).

    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
}

mtag_t active_mspace(void) { return active_space_mtag(); }

mtag_t switch_mspace(mtag_t mtag) {
    mtag_t prev;

    prev = csrrw_satp(mtag);
    sfence_vma();
    return prev;
}

// Copies all pages and page tables from the active memory space into newly allocated memory.
mtag_t clone_active_mspace(void) {
    // FIXME

    // We are cloning the current address spaces root tbale which means we need to get that first
    struct pte * curr_root_table = active_space_ptab();

    // Now use the clone helper
    struct pte * new_root_table = ptab_clone(curr_root_table);

    // Not using multiple ASIDs so pass 0 through and takes the phys addr of the new root table and returns the mtag version of it
    return ptab_to_mtag(new_root_table, 0);
}

// Unmaps and frees all non-global pages from the active memory space.
void reset_active_mspace(void) {
    // FIXME

    // To reset, we use the helper function but we need the root table again
    struct pte * root_table = active_space_ptab();

    // Use the helper to clear, unmap and free the non-global pages
    ptab_reset(root_table);

    // Use the given sfence_vma to flush the TLB to get rid odd of old entries in the hardware
    sfence_vma();
}

// Switches memory spaces to main, unmaps and frees all non-global pages from the previously active memory space.
mtag_t discard_active_mspace(void) {
    // FIXME

    // Completelty discard the curr addr space and swith to the main addr space
    // Need the root table again
    struct pte * root_table = active_space_ptab();

    // Get the current satp value in mtag form to compare and check if it is the main page table
    // mtag_t to_discard = active_space_mtag();

    // Now check to make sure that we are not deleting the main page table
    if(root_table != main_pt2){

        // Use the helper
        ptab_discard(root_table);
    }

    // Now swictch to the main page table as we deleted the root one
    csrw_satp(main_mtag);

    // Flush out the old TLB entries associated with the old satp
    sfence_vma();

    // Return the tag corresponding to the main memory space
    return main_mtag;
}

// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.

// We currently map 4K pages only. At some point it may be disirable to support
// mapping megapages and gigapages.

void *map_page(uintptr_t vma, void *pp, int rwxug_flags) {
    // FIXME

    // Just as we did for the freeing of the pages, need to make sure that the vma is aligned
    // Use the same logic as we did to check start addr
    // In order for it to be page aligined, it must start exactly on a page boundary, lower 12 bits must always be 0
    // So create a bitmask for just those 12 bits
    // Then AND the actual 12 bits with thsi bitmask of just 1s and if they do not come out as 0, then you know one of those bits is not 0
    // This means its unaligned
    assert((vma & ((uintptr_t) PAGE_SIZE -1)) == 0);

    // Check that the physical pointer is page aligned as well, similar process as above
    assert(((uintptr_t) pp & (PAGE_SIZE - 1)) == 0);

    // Now make sure the virtual address is wellformed
    // Need to make sure its valid Sv39 by using the wellformed helper
    assert(wellformed(vma));
    
    // Now check that pp is not null, make sure that its mapping to an actual physical page, not NULL
    assert(pp != NULL);

    // Now to actually insert the mapping, we need to start with the root table for Sv39
    // Use active_space_ptab which returns the address of hte page table corresponding to the active memmory space
    // Set this address as its the root
    struct pte * root_table = active_space_ptab();

    // Now convert the virtual address into the virtual page number
    // Use the VPN macro to convert
    unsigned long vpn = VPN(vma);

    // Now use the ptab_insert helper whicb actualy inserts the mapping
    // This helper does all the page table walking
    ptab_insert(root_table, vpn, pp, rwxug_flags);

    // Return the virtual address just mapped
    return (void *) vma;
}

// Adds a range of contiguous pages with provided virtual memory address, size, and flags to page table.
void *map_range(uintptr_t vma, size_t size, void *pp, int rwxug_flags) {
    // FIXME

    // This will map a range of memmory isntead of just a page so definitely reference map_page
    // Start by chcking the value of size as it must be non-zero, if zero there is nothing to map
    if(size == 0){

        return (void *) vma;
    }

    // Now check theat the address we are given in the parameter is page aligned, same process as before
    // In order for it to be page aligined, it must start exactly on a page boundary, lower 12 bits must always be 0
    // So create a bitmask for just those 12 bits
    // Then AND the actual 12 bits with thsi bitmask of just 1s and if they do not come out as 0, then you know one of those bits is not 0
    // This means its unaligned
    assert((vma & ((uintptr_t) PAGE_SIZE -1)) == 0);

    // Check if pp is page aligned as well using the same process as above
    assert(((uintptr_t) pp & (PAGE_SIZE - 1)) == 0);

    // Now check that pp is not null, make sure that its mapping to an actual physical page, not NULL
    assert(pp != NULL);
    
    // Now make sure the virtual address is wellformed
    // Need to make sure its valid Sv39 by using the wellformed helper
    assert(wellformed(vma));

    // Now compute the last byte address of the mapping to verify that the entire interval is valid
    uintptr_t end_vma = vma + (size-1);

    // Now check to make sure that its not overflowing and that the range is valid
    assert(end_vma >= vma);

    // Check that the ending address is wellformed as we did with vma
    assert(wellformed(end_vma));

    // Once we have checked that everything is valid, now to find the number of pages to cover the mapping
    // To compute the number of pages, we do the ceiling of size / PAGE_SIZE
    // ceiling of a/b is (a + b - 1)/b and >> 12 divides by page size
    size_t num_pages = (size + PAGE_SIZE - 1) >> PAGE_ORDER;

    // Now we walk tbrough each page to map the PTE
    for(size_t i = 0; i < num_pages; ++i){

        // Find the vma for each page
        uintptr_t page_vma = vma + (i << PAGE_ORDER);

        // Same thing but this time find the pp for each page
        void * page_pp = (void *)((uintptr_t)pp + (i << PAGE_ORDER));

        // Now map each page using the map_page function
        map_page(page_vma, page_pp, rwxug_flags);
    }

    // Return the starting vma of the mapped region
    return (void *) vma;
}
// Allocates memory for and maps a range of pages starting at provided virtual memory address. 
// Rounds up size to be a multiple of PAGE_SIZE
void *alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
    // FIXME

    // Extremely similar to map pages and map ranges, the difference being now we need to allocate pages as well
    // allocates pages from th elist and then maps the pages at the vma all in one function
    // So it will be a similar implementation

    // Same starting code from map_range
    // This will map a range of memmory isntead of just a page so definitely reference map_page
    // Start by chcking the value of size as it must be non-zero, if zero there is nothing to map
    if(size == 0){

        return (void *) vma;
    }

    // Now check theat the address we are given in the parameter is page aligned, same process as before
    // In order for it to be page aligined, it must start exactly on a page boundary, lower 12 bits must always be 0
    // So create a bitmask for just those 12 bits
    // Then AND the actual 12 bits with thsi bitmask of just 1s and if they do not come out as 0, then you know one of those bits is not 0
    // This means its unaligned
    assert((vma & ((uintptr_t) PAGE_SIZE -1)) == 0);
    
    // Now make sure the virtual address is wellformed
    // Need to make sure its valid Sv39 by using the wellformed helper
    assert(wellformed(vma));

    // Compute how many papes we need, same process from map_range
    // Once we have checked that everything is valid, now to find the number of pages to cover the mapping
    // To compute the number of pages, we do the ceiling of size / PAGE_SIZE
    // ceiling of a/b is (a + b - 1)/b and >> 12 divides by page size
    size_t num_pages = (size + PAGE_SIZE - 1) >> PAGE_ORDER;

    // Now allocate the physical pages from the free chunk list using alloc_phys_pages
    // Panic handled by alloc
    // Store as pp to map with
    void * pp = alloc_phys_pages((unsigned int) num_pages);

    // Now do the same checks for pp to make sure its aligned and defined
    // Check if pp is page aligned as well using the same process as above
    assert(((uintptr_t) pp & (PAGE_SIZE - 1)) == 0);

    // Now check that pp is not null, make sure that its mapping to an actual physical page, not NULL
    assert(pp != NULL);

    // Now map the physical range at vma using the map_range we implemented above
    map_range(vma, size, pp, rwxug_flags);

    // Return the starting address of the mapped region
    return (void *) vma;
}

// Sets passed flags for pages in range. Rounds up size to be a multiple of PAGE_SIZE.
void set_range_flags(const void *vp, size_t size, int rwxug_flags) {
    // FIXME

    // This functions follows the exact same setup as map range.
    // All we are doing is instead of mapping a pafe in each range, we are adjusting the flags
    // So the code is pretty much the exact same

    // Start by chcking the value of size as it must be non-zero, if zero then return
    if(size == 0){

        return;
    }

    // Define what vma by converting the virtual pointer into the address
    uintptr_t vma = (uintptr_t) vp;

    // Now check theat the address we are given in the parameter is page aligned, same process as before
    // In order for it to be page aligined, it must start exactly on a page boundary, lower 12 bits must always be 0
    // So create a bitmask for just those 12 bits
    // Then AND the actual 12 bits with thsi bitmask of just 1s and if they do not come out as 0, then you know one of those bits is not 0
    // This means its unaligned
    assert((vma & ((uintptr_t) PAGE_SIZE -1)) == 0);
    
    // Now make sure the virtual address is wellformed
    // Need to make sure its valid Sv39 by using the wellformed helper
    assert(wellformed(vma));

    // Now compute the last byte address of the range to verify that the entire interval is valid
    uintptr_t end_vma = vma + (size - 1);

    // Now check to make sure that its not overflowing and that the range is valid
    assert(end_vma >= vma);

    // Check that the ending address is wellformed as we did with vma
    assert(wellformed(end_vma));

    // Once we have checked that everything is valid, now to find the number of pages to cover the range
    // To compute the number of pages, we do the ceiling of size / PAGE_SIZE
    // ceiling of a/b is (a + b - 1)/b and >> 12 divides by page size
    size_t num_pages = (size + PAGE_SIZE - 1) >> PAGE_ORDER;

    // Now is where things differ a bit, we need to convert the page number into the starting address
    // In our loop, we use ptab)adjust which takes the vpn as an input and this needs to be updated
    unsigned long starting_vpn = VPN(vma);

    // We also need to get the root table of the address space as we did for map page
    struct pte * root_table = active_space_ptab();

    // Now we walk tbrough each page to adjust the flags
    for(size_t i = 0; i < num_pages; ++i){

        // Advance the vpn by exactly one at each setp so that when used by the adjust, its adjusting the correct flags
        unsigned long vpn = starting_vpn + i;

        // Now use the helper function ptab_adjust to do the heavy work for us as we implement it
        ptab_adjust(root_table, vpn, rwxug_flags);
    }

}

// Unmaps a range of pages starting at provided virtual memory address and frees the pages. 
// Rounds up size to be a multiple of PAGE_SIZE.
void unmap_and_free_range(void *vp, size_t size) {
    // FIXME

    // This will basically do the exact opposite of alloc and map by unmapping and freeing
    // This means that the setup is going to very similar, we are going to have to make our checks
    // After checking validity and alignment, loop through each page in the range and remove/unmap
    
    // This functions follows the exact same setup as map range.
    // All we are doing is instead of mapping a pafe in each range, we are adjusting the flags
    // So the code is pretty much the exact same

    // Start by chcking the value of size as it must be non-zero, if zero then return
    if(size == 0){

        return;
    }

    // Define what vma by converting the virtual pointer into the address
    uintptr_t vma = (uintptr_t) vp;

    // Now check theat the address we are given in the parameter is page aligned, same process as before
    // In order for it to be page aligined, it must start exactly on a page boundary, lower 12 bits must always be 0
    // So create a bitmask for just those 12 bits
    // Then AND the actual 12 bits with thsi bitmask of just 1s and if they do not come out as 0, then you know one of those bits is not 0
    // This means its unaligned
    assert((vma & ((uintptr_t) PAGE_SIZE -1)) == 0);
    
    // Now make sure the virtual address is wellformed
    // Need to make sure its valid Sv39 by using the wellformed helper
    assert(wellformed(vma));

    // Now compute the last byte address of the range to verify that the entire interval is valid
    uintptr_t end_vma = vma + (size - 1);

    // Now check to make sure that its not overflowing and that the range is valid
    assert(end_vma >= vma);

    // Check that the ending address is wellformed as we did with vma
    assert(wellformed(end_vma));

    // Once we have checked that everything is valid, now to find the number of pages to cover the range
    // To compute the number of pages, we do the ceiling of size / PAGE_SIZE
    // ceiling of a/b is (a + b - 1)/b and >> 12 divides by page size
    size_t num_pages = (size + PAGE_SIZE - 1) >> PAGE_ORDER;

    // Now is where things differ a bit, we need to convert the page number into the starting address
    // In our loop, we use ptab)adjust which takes the vpn as an input and this needs to be updated
    unsigned long starting_vpn = VPN(vma);

    // We also need to get the root table of the address space as we did for map page
    struct pte * root_table = active_space_ptab();

    // Now we crate the loop, which will loop through each region and then remove the mapping
    // After removing the mapping, it will call free phys page to free from memory
    for(size_t i = 0; i < num_pages; ++i){

        // Advance the vpn by exactly one at each setp so that when used by the adjust, its adjusting the correct flags
        unsigned long vpn = starting_vpn + i;

        // To actually remove the mapping, call the ptab_remove helper that unmaps the page
        // The ptab_remove function returns the physical pointer that was mapped there
        void * pp = ptab_remove(root_table, vpn);

        // Now free the pages using free phys pages using pp
        // This only occurs if pp is not NUll
        if(pp != NULL){

            free_phys_page(pp);
        }
    }
}

// Checks that pointer is wellformed and pointer + len does not wrap around zero, 
// then iterates over pages in range, confirming the pages are mapped and have the passed flags set
int validate_vptr(const void *vp, size_t len, int rwxu_flags) {
    // FIXME

    // This function checks the validity of a virtual memory range
    // Similar to the checks that we did in mapping functions but this also check flag permissions
    // Checks to make sure that everything is valid before the kernel reads or writes to it

    // Check length first, if zero then its an empty ran ge which means an immediate success
    if(len == 0){

        // Return 0 for a succesful check
        return 0;
    }

    // Otherwise, if there is a length, then there has to be multiple other checkts to return 0
    // First get the starting address as an interger type so that we can use it
    uintptr_t start_addr  = (uintptr_t) vp;

    // Now check page alignment as we did in the previous functions
    //if((start_addr & (PAGE_SIZE - 1)) != 0){

        // Return einval to signify an invalid argument
        //return -EINVAL;
    //}

    // Now we need to make sure that it does not overflow
    // To do this, need to find the end of the range, this is just the start plus the length
    // This gives us one past the last byte to check the overflow
    uintptr_t end_addr = start_addr + len;

    if(start_addr > end_addr){
        
        return -EINVAL;
    }
    // Now we need to make sure that the address are wellformed
    // Check start addr and end addr -1
    // WE check end addr - 1 because end is one past the last byte for overflow check
    // The last actual address in the range is -1
    if(!wellformed(start_addr) || !wellformed(end_addr - 1)){

        return -EINVAL;
    }
    
    // Now that we have checked that the address exists, the range is valid, and the addresses are wellformed
    // We must now check that every page is actaullay mapped in the current address
    // Also check that each page is mapped by a leaf PTE and has all the correct permissions
    // To start, get the root page table to validate through that
    struct pte * root_table = active_space_ptab();

    // Now to loop through each of the addresses, use the page indicies so convert the addresses
    // Use the VPN macro
    unsigned long vpn_start = VPN(start_addr);
    unsigned long vpn_end = VPN(end_addr - 1);

    // Now loop through the every page in the range, inclusive of vpn_end as we already -1
    for(unsigned long i = vpn_start; i <= vpn_end; ++i){

        // Use the ptab_adjust to find the pte slot fot the VPN
        struct pte * pte = ptab_fetch(root_table, i);

        // Check if its unmapped,  if it is the range is not correct or safe, so reject by returning -EACCESS
        if(pte == NULL){

            return -EACCESS;
        }

        // Now check the two conditionos for a valid PTE. The V flag must be 1 and the it should be a leaf
        // Only leaf entries have access
        if(!PTE_VALID(* pte)){

            return -EACCESS;
        }

        if(!PTE_LEAF(* pte)){

            return -EACCESS;
        }

        // Last check would be to check all the flags are present
        // Compare the PTE's flags with rwxu_flags and if anything differs, reject
        // Do this by masking the ptes flags and mask them withbt he caller ones and checking for different bits
        if(((int) pte->flags & rwxu_flags) != rwxu_flags){

            return -EACCESS;
        }
    }

    // If gotten to this point, then every page is mapped by a valid leaf PTE with required permission, success
    return 0;
}

// Checks that pointer is wellformed and the given string is valid. 
// Since the length of the string is unknown, we iterate through all characters of the string until \0 terminator, 
// confirming that the pages are mapped and have the passed flags set.
int validate_vstr(const char *vs, int rug_flags) {
    // FIXME

    // String checker for the kerne, checks if user stirng is safe to read
    // First check if the string pointer is not valid
    if(vs == NULL){

        return -EINVAL;
    }

    // As we did before, cehck to make tsure that the address is wellformed, cast the pointer to an int to make the check
    // If not wellformed, return an error
    if(!wellformed((uintptr_t) vs)){

        return -EINVAL;
    }

    // Get the root table to validate each byte as we have done
    struct pte * root_table = active_space_ptab();

    // Create an iterator that will walk through the string until we see the \0 terminator
    const char *str_loop = vs;

    // Infinite loop that will only end once \0 is reached
    for(;;){

        // Find the current address of the current element of the string and turn onto an integer address
        uintptr_t addr = (uintptr_t) str_loop;

        // Check to make sure there is no overflow with the string
        // if addr + 1 < addr, then we know that it wrapped around which overflows
        if(addr + 1 < addr){

            return -EINVAL;
        }
        
        // Now check to make sure the address is wellformed
        if(!wellformed(addr)){

            return -EINVAL;
        }

        // Similar process from the vptr validation
        // Translate the address into the VPN using the macro
        unsigned long vpn = VPN(addr);

        // Use the ptab_adjust to find the pte slot fot the VPN
        struct pte * pte = ptab_fetch(root_table, vpn);

        // Same process for the vstr function compared to vptr, only difference is caller flags
        // Check if its unmapped,  if it is the range is not correct or safe, so reject by returning -EACCESS
        if(pte == NULL){

            return -EACCESS;
        }

        // Now check the two conditionos for a valid PTE. The V flag must be 1 and the it should be a leaf
        // Only leaf entries have access
        if(!PTE_VALID(* pte)){

            return -EACCESS;
        }

        if(!PTE_LEAF(* pte)){

            return -EACCESS;
        }

        // Last check would be to check all the flags are present
        // Compare the PTE's flags with rug_flags and if anything differs, reject
        // Do this by masking the ptes flags and mask them withbt he caller ones and checking for different bits
        if(((int) pte->flags & rug_flags) != rug_flags){

            return -EACCESS;
        }

        // Load the character to check for the terminator character
        char term_check = * str_loop;
        
        // Check for it
        if(term_check == '\0'){

            break;
        }

        // Go to the next element
        ++str_loop;
    }

    // If passed, then a success so return 0
    return 0;
}

// Allocates a single new page using alloc_phys_pages().
void *alloc_phys_page(void) {
    // FIXME

    // One line of code, just use the alloc_phys_pages and have count be 1
    return alloc_phys_pages(1);
}

void free_phys_page(void *pp) {
    // FIXME
    
    // One line of code, just use the free_phys_pages and have count be 1
    free_phys_pages(pp, 1);
}

// Allocates the passed number of physical pages from the free chunk list
// Finds smallest chunk that fits the requested number of pages. 
// If chunk exactly matches the number of pages requested, 
// removes chunk from free chunk list, otherwise breaks off the component 
// of chunk that matches requested number of pages. 
// Panics if no chunk can be found that satisfies the request.
void *alloc_phys_pages(unsigned int cnt) {
    // FIXME

    // Define 4 variables pointing to the struct page_chunk
    // -> curr, prev, best, and best_prev
    // curr and prev are iteration pointers when traversing the free chunk list
    // best and best_prev are initialized to track the best chunk to fit the given cnt
    // Result will hold the pointer to the allocated pages
    struct page_chunk * curr;
    struct page_chunk * prev;
    struct page_chunk * best;
    struct page_chunk * best_prev;
    void * result;

    // We dont need base_addr and pages_before as we know each struct page_chunk lives at the physical start address of the free chunk it describes

    // Define a variable to holld the computed base address of the chunk that is the best fit for the pages
    // uintptr_t base_addr;

    // pages_before holds the number of free pages in all chunks befre best to compute the bae address
    // unsigned long pages_before;

    // Check if cnt is 0, if it is then return NULL as there are no pages to allocate
    if(cnt == 0){

        return NULL;
    }
    
    // Check that the chunk list actually has chunks to allocate pages for
    // Check that there is a chunk to allocate. If the list is NULL, panic as there are no free chunks
    if(free_chunk_list == NULL){

        panic("alloc_phys_pages: no free chunks available -> free_chunk_list ==  NULL");
    }

    // Documnetation says that we need to find the smallest chunk that fits the requested number of pages
    // So we need to walk the list from the had and find the smallest chunk in general for now
    // Set prev to null and the curr to the head of the list to walk the list
    prev = NULL;
    curr = free_chunk_list;

    // Inititalize boht best and best_prev
    best = NULL;
    best_prev = NULL;

    // Create a while loop condition for curr to see if there are nodes still left
    while(curr != NULL){

        // Only look through chunks that have a page count large enough to the support the cnt given in the parameter
        if(curr->pagecnt >= cnt){

            // Now check if the current node is the best fit for the count and the chunks available
            // best == NULL checks if this is the first iteration and curr->pagecnt < best->pagecnt compares for the best fit
            if(best == NULL || curr->pagecnt < best->pagecnt){

                // If this condition passes through, then curr is a better fit as it is smaller
                best = curr;
                
                // Set the best_prev to prev to remeber the previous node of best
                best_prev = prev;
            }
        }

        // Now advance to check the next node in the list, set prev to the curr and curr to the curr next
        prev = curr;
        curr = curr->next;

    }
    
    // After the loop, check and make sure best holds a node that would fit. If not, then panic
    if(best == NULL){

        panic("alloc_phys_pages: not enough pages for cnt");
    }

    // This is where the code changes, we dont need the pages_before or the starting addr
    // We are going to compute the addresses based urely on the chunk pointer
    // So start with the chunk struct addr, beginning of the chunk in bytes
    uintptr_t start = (uintptr_t) best;

    // Compute how many bytes are in the chunk
    // best->pagecnt is the number of pages and then left shifted byt PAGE_ORDER is the same as multiplyiny by PAGE_SIZE
    uintptr_t chunk_bytes = ((uintptr_t) best->pagecnt << PAGE_ORDER);

    // Now we need to compute how many bytes to allocate
    // cnt is the number of pages requested and leftshifted basically multiplying by 12
    uintptr_t alloc_bytes = ((uintptr_t) cnt << PAGE_ORDER);

    // Make sure the chunk is big enough for the bytes needed for allocation
    assert(chunk_bytes >= alloc_bytes);

    // Now handle the case where chunks page count exacltly fits the requested page count
    if(best->pagecnt == cnt){

        // result would hold the chunks start address as the allocation starts at start
        result = (void *) start;

        // Now we should remove the chunk fromm the free list
        if(best_prev == NULL){

            // If best_prev was null, that means that the best was the head
            // So the header of the free list should be the next
            free_chunk_list = best->next;
        }

        // Otherwise, we just have to skip over
        else{

            // Just skipping over
            best_prev->next = best->next;
        }

    }

    // We do not use kfree as I did before as we need to free it all logically
    // Now we need to take care of the partial allocation case
    // The free region for this chunk is start to start + chunk_bytes and we need to update it with alloc_bytes
    // Allocate from the end of the chunk
    else{

        // Store the starting address of the allocated end regiom
        uintptr_t result_addr = start + (chunk_bytes - alloc_bytes);

        // Set the result to the result_addr
        result = (void *) result_addr;

        // Now update the pagecnt as it now represnets a smaller chunk
        best->pagecnt -= cnt;
    }

    // Now return the void pointer that holds the starting address of the allocated block
    return result;

    // Deleted the old code, used base_addr which caused overlap
}

// Adds chunk consisting of passed count of pages at passed pointer back to free chunk list.
// Parameters
// -> pp	Physical address of memory region to add back to free chunk list
// -> cnt	Number of pages being freed
void free_phys_pages(void *pp, unsigned int cnt) {
    // FIXME

    // We are going to need to traverse the list so a similar setup to alloc without best
    // Inititalize prev to be NULL as we are going to set curr to be the head of the list
    struct page_chunk * prev = NULL;

    // Set curr to be the head
    struct page_chunk * curr = free_chunk_list;

    // Create a new chunk pointer that will hold the newly freed region, not the actual data, basically just a label to it
    // struct page_chunk * new_chunk;

    // Track the number of pages before curr just as alloc
    // unsigned long pages_before = 0;

    // Chefck that there cnt is not 0 or that the address is not NULL to make sure there are pages to free
    if(cnt == 0 || pp == NULL){

        // Return if true as nothing to do
        return;
    }

    // Define start_addr as the integer address from pp
    uintptr_t start_addr = (uintptr_t) pp;

    // Make sure that the start addr is page aligned
    // In order for it to be page aligined, it must start exactly on a page boundary, lower 12 bits must always be 0
    // So create a bitmask for just those 12 bits
    // Then AND the actual 12 bits with thsi bitmask of just 1s and if they do not come out as 0, then you know one of those bits is not 0
    // This means its unaligned
    assert((start_addr & ((uintptr_t) PAGE_SIZE - 1)) == 0);

    // Verify that the address being freed is not below the start of the memory
    assert(start_addr >= (uintptr_t) free_base_addr);

    // Compute the end address of the freed region
    // Connvert the pages into bytes as we did above
    uintptr_t end_addr = start_addr + ((uintptr_t) cnt << PAGE_ORDER);

    // Verify that the freed region is at or beloe the boundary for the end of the RAM memory
    assert(end_addr <= (uintptr_t) RAM_END);

    // Find the page index as we are going to be switching from byte address
    // Do this as the allocator granulity is pages and we will need to free exacr numbers of pages, not bytes
    // So find the starting page index by subtracting tha start to the base to get it in bbytes before righ shifting by 12
    //unsigned long free_start_pg_idx = (start_addr - (uintptr_t) free_base_addr) >> PAGE_ORDER;

    // Now find the end index by adding the paramter cnt to the start
    //unsigned long free_end_pg_idx = free_start_pg_idx + cnt;

    // FIX: we do need the indicies as we know where the chunk starts

    // Now we need to traverse the list until we find the rigt spot in the list to insert this newly freed region
    // Walk the list which is sorted by the start address to find the new interval
    while(curr != NULL){

        // Start with the address of the struct page_chunk and store it in curr_start
        uintptr_t curr_start = (uintptr_t) curr;

        // Now also find the end by adding the size of the bytes
        uintptr_t curr_end = curr_start + ((uintptr_t) curr->pagecnt << PAGE_ORDER);

        // Now lets take a look at the first case where the new region is strictly before this chunk
        // What this means is that we have reached the first existing chunk that starts after the new one ends
        // This means the new chunk should be inserted right before curr
        // So now that we know where, we break the loop
        if(curr_start >= end_addr){

            break;
        }

        // The second case is that the new region is stricly after the chunk
        // In that case we have still not found the right spot to insert so we continue walking throught the code
        if(start_addr >= curr_end){

            // Now we need to update to keep moving through the list
            prev = curr;

            // Move to the next chunk
            curr = curr->next;

            // Move forward
            continue;
        }
    
        // Third case is that the freed region overlaps in the current chunk
        // If this is the case, then that means that there are two frees for the same region
        // Or it could be that the cnt is invalid, either way its invalid
        panic("free_phys_pages: overlapping pages");
    }

    // Insert a new chunk thats struct lives at the start of the free region
    struct page_chunk * new_freed_chunk = (struct page_chunk *) start_addr;

    // This chink describes cnt pages so update it
    new_freed_chunk->pagecnt = cnt;

    // Now we need to maintain the list, make sure that the list remains sorted
    // Do this by the physical addresses
    // If there is no prev chunk, then the new freed region should be put before the current head
    if(prev == NULL){

        // Set the new chunk to be in the head location and for it to be the head
        new_freed_chunk->next = free_chunk_list;
        free_chunk_list = new_freed_chunk;
    }
    
    // Else, but the new chunk in between prev and curr
    else{

        // Put it right in between, where it should be
        new_freed_chunk->next = curr;
        prev->next = new_freed_chunk;
    }
}

// Counts the number of pages remaining in the free chunk list.
unsigned long free_phys_page_count(void) {
    // FIXME

    // Struct page chunk pointer that is initialized to the head of the free list
    struct page_chunk * chunk = free_chunk_list;

    // Running tottal taht will hold the number of free pages as we go through the loop
    unsigned long total = 0;

    // Walk the free chunk list and sum all of the page count fields to total
    // The condition being that chunk, which points to the head, is not null, therefore there are still chunks to traverse
    while(chunk != NULL){

        // Add to the running total
        total += chunk->pagecnt;

        // Go to the next chunk
        chunk = chunk->next;
    }

    // Return the total number of pages left in the list
    return total;
}
// Called by handle_umode_exception() in excp.c to handle U mode load and store page faults. 
// It returns 1 to indicate the fault has been handled (the instruction should be restarted) and 
// 0 to indicate that the page fault is fatal and the process should be terminated.
// From Piazza: the function handle_umode_page_fault should not panic when a user tries to access memory 
// outside of user memory space
// Instead, it should return 0 to indicate that it has not been handled.
int handle_umode_page_fault(struct trap_frame *tfr, uintptr_t vma) {
    // FIXME

    // Tnhis function does not use the parametners so to not get a compiler, cast
    (void) tfr;
    (void) vma;

    // Now we create the conditional debugging logic
    // If MEMORY_DEBUG is defines, then the compiler sees the debug line
    // If not, then it will detlete it
    // Debug that shows that there is a page fault
    #ifdef MEMORY_DEBUG

        // Will print the address of the fault
        debug("handle_umode_page_fault: U-mode page fault at %p, not handled", (void *) vma);
    #endif

    // Fault not handeled, treated as fatal for the process
    return 0;  // no handled
}

/**
 * @brief Reads satp to retrieve tag for active memory space
 * @return Tag for active memory space
 */
mtag_t active_space_mtag(void) { return csrr_satp(); }

/**
 * @brief Constructs tag from page table address and address space identifier
 * @param ptab Pointer to page table to use in tag
 * @param asid Address space identifier to use in tag
 * @return Memory tag formed from paging mode, page table address, and ASID
 */
static inline mtag_t ptab_to_mtag(struct pte *ptab, unsigned int asid) {
    return (((unsigned long)PAGING_MODE << RISCV_SATP_MODE_shift) |
            ((unsigned long)asid << RISCV_SATP_ASID_shift) | pagenum(ptab) << RISCV_SATP_PPN_shift);
}

/**
 * @brief Retrives a page table address from a tag
 * @param mtag Tag to extract page table address from
 * @return Pointer to page table retrieved from tag
 */
static inline struct pte *mtag_to_ptab(mtag_t mtag) { return (struct pte *)((mtag << 20) >> 8); }

/**
 * @brief Returns the address of the page table corresponding to the active memory space
 * @return Pointer to page table extracted from active memory space tag
 */
static inline struct pte *active_space_ptab(void) { return mtag_to_ptab(active_space_mtag()); }

/**
 * @brief Constructs a physical pointer from a physical page number
 * @param n Physical page number to derive physical pointer from
 * @return Pointer to memory corresponding to physical page
 */
static inline void *pageptr(uintptr_t n) { return (void *)(n << PAGE_ORDER); }

/**
 * @brief Constructs a physical page number from a pointer
 * @param p Pointer to derive physical page number from
 * @return Physical page number corresponding to pointer
 */
static inline unsigned long pagenum(const void *p) { return (unsigned long)p >> PAGE_ORDER; }

/**
 * @brief Checks if bits 63:38 of passed virtual memory address are all 1 or all 0
 * @param vma Virtual memory address to check well-formedness of
 * @return 1 if pointer is well-formed, 0 otherwise
 */
static inline int wellformed(uintptr_t vma) {
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits + 1));
}

/**
 * @brief Constructs a page table entry corresponding to a leaf
 * @details For our purposes, a leaf PTE has the A, D, and V flags set
 * @param pp Physical address to set physical page number of PTE from
 * @param rwxug_flags Flags to set on PTE
 * @return PTE initialized with proper flags and PPN
 */
static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags) {
    return (struct pte){.flags = rwxug_flags | PTE_A | PTE_D | PTE_V, .ppn = pagenum(pp)};
}

/**
 * @brief Constructs a page table entry corresponding to a page table
 * @param pt Physical address to set physical page number of PTE from
 * @param g_flag Flags to set on PTE (should either be G flag or nothing)
 * @return PTE initialized with proper flags and PPN
 */
static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag) {
    return (struct pte){.flags = g_flag | PTE_V, .ppn = pagenum(pt)};
}

/**
 * @brief Returns an empty pte
 * @return An empty pte
 */
static inline struct pte null_pte(void) { return (struct pte){}; }

// Implementing my own helpers and the helpers the code gave to us
/**
 * @brief Returns a pointer to the child page table for a non-leaf PTE
 * @param entry A pointer whose ppn field will corespond to the child page table
 * @return Pointer to the child page table that corresponds to the entry's ppn
*/
static inline struct pte * pte_child(const struct pte * entry){

    // Return the pointer to the child page table that corresponds to the ppn of entry
    return (struct pte *) pageptr(entry->ppn);
}

// Now implementing the functions the code gave us but without the implementations

// Ptab_reset will recursively unmap and free all the non-gloval pages from this page table
// It wont dree the actual page table, that is for discard
static void ptab_reset(struct pte * ptab){

    // For each valid non-global, leaf entry, it will free the phys page and clear the PTE
    // for the non-leage entries, it will recursively go throughn the child table, and do the same thing
    // Start off by loping through each of the PTE's in the page table
    for(unsigned int i = 0; i < PTE_CNT; ++i){

        // Store  the current element or entire, ptab[i] in curr to reference later
        struct pte curr = ptab[i];

        // Now check that the PTE is valid using the macro
        if(!PTE_VALID(curr)){

            // If its not valid, skip the loop and go to next elem
            continue;
        }

        // Check if global as we only reset non-global entries
        if(PTE_GLOBAL(curr)){

            // If global, move forward to next elem
            continue;
        }

        // Now we have leaf cases
        // If it its a leaf table, we know its non-global and valid so we can unmap adn free the pages
        if(PTE_LEAF(curr)){

            // Free by storing the phyciscal page number by using the helper that converts ppn to ptr addr
            void * pp = pageptr(curr.ppn);

            // Free using free_phys_page
            free_phys_page(pp);

            // Set the current page table entry to null
            ptab[i] = null_pte();
        }

        // Else, if its a non-leage, then free the child page table
        else{

            // Get the child table pointer
            struct pte * child = pte_child(&curr);

            // Reset the child table recursively
            ptab_reset(child);

            // Now that the child table has been emptied, free the child page
            free_phys_page(child);

            // Set the current table to null
            ptab[i] = null_pte();
        }
    }
}

// Ptab_discard will recursively discard rhe address space at ptab
// Frees all non global mappings and page tables and then also frees ptab as well
static void ptab_discard(struct pte * ptab){

    // Similar process just as ptab_reset

    // For each valid non-global, leaf entry, it will free the phys page and clear the PTE
    // for the non-leage entries, it will recursively go throughn the child table, and do the same thing
    // Start off by loping through each of the PTE's in the page table
    for(unsigned int i = 0; i < PTE_CNT; ++i){

        // Store  the current element or entire, ptab[i] in curr to reference later
        struct pte curr = ptab[i];

        // Now check that the PTE is valid using the macro
        if(!PTE_VALID(curr)){

            // If its not valid, skip the loop and go to next elem
            continue;
        }

        // Check if global as we only reset non-global entries
        if(PTE_GLOBAL(curr)){

            // If global, move forward to next elem
            continue;
        }

        // Now we have leaf cases
        // If it its a leaf table, we know its non-global and valid so we can unmap adn free the pages
        if(PTE_LEAF(curr)){

            // Free by storing the phyciscal page number by using the helper that converts ppn to ptr addr
            void * pp = pageptr(curr.ppn);

            // Free using free_phys_page
            free_phys_page(pp);

            // Set the current page table entry to null
            ptab[i] = null_pte();
        }

        // Else, if its a non-leaaf, then we recursively go to discard the child
        else{

            // Get the child table pointer
            struct pte * child = pte_child(&curr);

            // Reset the child table recursively
            ptab_discard(child);

            // Set the current table to null
            ptab[i] = null_pte();
        }
    }

    // Free the page table itself unless it is the main root table
    if(ptab != main_pt2){

        free_phys_page(ptab);
    }
}

// Ptab_clone makes a new copy of the address space including all the different levels
// The cloned copy will sahre mappings and address space
// For global entries, reuses the same PTE
// For non-global leaf entries, allocated a physucal page, copies the contents and create sa new leaf PTE
// For non-global non-leaf, recursivelly call clone to cllone the child table and create a PTE in the new table to point to the child table
static struct pte * ptab_clone(struct pte * ptab){

    // Allocate a new page to become to hold the cloned table
    // Use the alloc phys page function
    void * ptab_page = alloc_phys_page();

    // Check that the allocation was valid
    assert(ptab_page != NULL);

    // Cast the void page pointer to a struct pte pointer which treats the page as an array of emtries
    struct pte * new_ptab = (struct pte *) ptab_page;

    // Now zero it in memoery so that it is currently seen as unmapped
    memset(new_ptab, 0, PAGE_SIZE);

    // Nowwe can iterate over all of the entries in the original table so that we can copy it over
    for(unsigned int i = 0; i < PTE_CNT; ++i){

        // Store the current element into curr for for later reference
        struct pte curr = ptab[i];

        // Now check that the PTE is valid using the macro
        if(!PTE_VALID(curr)){

            // If its not valid, skip the loop and go to next elem
            continue;
        }

        // Check if global, then we copy the entire PTE struct into the new table
        if(PTE_GLOBAL(curr)){

            // Set the entire struct into that index
            new_ptab[i] = curr;
            // Move forward to next elem
            continue;
        }

        // Case for a non global leaf page
        if(PTE_LEAF(curr)){

            // We need to first start by cloingin the physical page
            // Convert the ppn into a pointer to the page using the helper
            void * old_page = pageptr(curr.ppn);

            // Create the new cloned page by allocating a page for it
            void * new_page = alloc_phys_page();

            // Now check that it was allocated properly
            assert(new_page != NULL);

            // Now use memcpy to make a full page copy of old page into new page
            memcpy(new_page, old_page, PAGE_SIZE);

            // Now we need to preserve the relevant flag bits so that we can set it to the new cloned page
            // The relevant flags are R, W, X, U, G, thats all we need so do a bitwise ANd with the current flag to get just them
            uint_fast8_t rwxug = (uint_fast8_t)(curr.flags & (PTE_R | PTE_W | PTE_X | PTE_U | PTE_G));

            // Use the leaf pte helper to make the same page with the same permission bits
            new_ptab[i] = leaf_pte(new_page, rwxug);
        }

        // If a non-leaf, non-global, then we clone the child table and keep the permissions for a gild one
        else{

            // Convert the ppn into a pointer to the page using the helper
            void * old_child = pageptr(curr.ppn);

            // Create the new cloned page by recursively calling
            void * new_child = ptab_clone(old_child);

            // Keep only the G bit as its the only one we need
            uint_fast8_t g_flag = (uint_fast8_t)(curr.flags & PTE_G);

            // Use the helper to create the page table PTE
            new_ptab[i] = ptab_pte(new_child, g_flag);
        }

    }

    // Return the cloned ptab
    return new_ptab;
}

// Ptab_fetch walks the page table and computes the index
// Look at the PTE and check validity, if leaf then found the mapping so return that PTE
// Otherwise walk through till the leaf
struct pte * ptab_fetch(struct pte * ptab, unsigned long vpn){

    // First we have to initialize a pointer for where we are currently on the table
    // Set to ptab initially
    struct pte * curr_pg = ptab;

    // Loop over the different levels using the Root level var
    for(int i = ROOT_LEVEL; i >= 0; --i){

        // Now we need to compute the index at this level
        // Use PT_INDEX to get the 9 bit VPN component
        unsigned int idx = PT_INDEX(i, vpn);

        // Now store the address of the PTE struct at the current index for reference later
        struct pte * curr_addr = &curr_pg[idx];

        // Condition of validity
        if(!PTE_VALID(* curr_addr)){

            return NULL;
        }

        // If it is either a leaf PTE or if its a level 0 PTE, then return
        if(PTE_LEAF(* curr_addr) || i == 0){

            return curr_addr;
        }

        // If not then continue to walk by descending to the child page table
        curr_pg = pte_child(curr_addr);
    }

    // Return Null if nothing is returned at this point
    return NULL;
}

// Ptab_insert takes in the root pahe table, the virtual page number, the physical page pointer and the flags
// It makes sire that the page table ahs a path down to level 0 for the specific vpn
// Inserts a mapping for vpn to pp given the flags and make sures the path table architecure is correct
static void ptab_insert(struct pte * ptab, unsigned long vpn, void * pp, int rwxug_flags){

    // Start with the current page table page as we traverse the pt tree
    struct pte * curr_pg = ptab;

    // Similar to above, we walk from the root level down to loop over the levels
    // This time we stop at level 1, not all the way to level 0
    // At level 0 we  install the leaf mapping
    for(int i = ROOT_LEVEL; i > 0; --i){

        // Now we need to compute the index at this level
        // Use PT_INDEX to get the 9 bit VPN component
        unsigned int idx = PT_INDEX(i, vpn);

        // Now store the address of the PTE struct at the current index for reference later
        struct pte * curr_addr = &curr_pg[idx];

        // If the PTE at the curr addr is not valid, then we need to allocate a new sub table
        if(!PTE_VALID(* curr_addr)){

            // Allocate a sub page table using the alloc phys page function
            void * sub_ptab = alloc_phys_page();

            // Make sure the allocation was proper
            assert(sub_ptab != NULL);

            // Cast the physical page to a PTE as that page will store an array of PTEs
            struct pte * child = (struct pte *) sub_ptab;

            // Now zero out the new table as there are no mappings yet
            memset(child, 0, PAGE_SIZE);

            // Now install the PTE into the parent
            // The page at the curr_addr is a valid non-leaf PTE pointing to the child tab;e
            * curr_addr = ptab_pte(child, 0);

            // Set the curr_addr page to bethe child so we can continue walking down
            curr_pg = child;
        }

        // If the current page is valid
        else{

            // Expecting a subtable, if it is a leaf then panic
            if(PTE_LEAF(* curr_addr)){

                panic("ptab_insert: leaf PTE found instead of subtable");
            }

            // Otherwise, we know it is a child page table so continute walking
            curr_pg = pte_child(curr_addr);
        }

    }

    // Now at level 0, we nede to install or in some cases replace the leaf mapping
    // curr_pg now points to the lvl 0 table so compute the index of it
    // Use PT_INDEX to get the 9 bit VPN component
    unsigned int idx_0 = PT_INDEX(0, vpn);

    // Define the PTE that will be the leaf where the actual mapping will go
    struct pte * leaf = &curr_pg[idx_0];
    
    // Now we need to do the valid leaf global checks
    // If there is an existing non-global leaf mapping, then we need to free the old pp
    if(PTE_VALID(* leaf) && PTE_LEAF(* leaf) && !PTE_GLOBAL(* leaf)){

        // Define the old pp by using the current leafs ppn
        void * old_pp = pageptr(leaf->ppn);

        // Free the old pp by using the free phys page funciton
        free_phys_page(old_pp);
    }

    // Now chcek the error case that it is valid but its not a leaf
    if(PTE_VALID(* leaf) && !PTE_LEAF(* leaf)){

        // Panic
        panic("ptab_insert: non-leaf PTE at lvl 0, panic");
    }

    // Now to actually create the new PTE, call upon the leaf_pte helper that builds it from the pp and the flags
    * leaf = leaf_pte(pp, (uint_fast8_t) rwxug_flags);
}

// For this helper it will return 0 if there is no mapping removed in the subtree, 1 if there was a mappring removed
// 1 also says there are still other valid entries, 2 says removed and there is nothing left in the table
static int ptab_remove_recursive_helper(struct pte * ptab, int level, unsigned long vpn, void **pp_out){

    // First thing we need to do is index into the table as we have done multiple times before
    // Use PT_INDEX to get the 9 bit VPN component
    unsigned int idx = PT_INDEX(level, vpn);

    // Now store the address of the PTE struct at the current index for reference later
    struct pte * curr_pte = &ptab[idx];

    // If unmapped then nothing to remove
    if(!PTE_VALID(* curr_pte)){

        return 0;
    }

    // If it is a leaf mapping or at level 0, the curr pte defines the mapping to remove
    if(PTE_LEAF(* curr_pte) || level == 0){

        // In case its level 0 but not a leaf which means there is no actual mapping here.
        if(!PTE_LEAF(* curr_pte)){

            return 0;
        }

        // Now we need to get the physical page pointer so that we can clear it and then check back on it
        if(pp_out != NULL){

            // Store the physciall address in the pointer and then we know what page was mapped
            *pp_out = pageptr(curr_pte->ppn);
        }

        // Now clear the PTE
        * curr_pte = null_pte();

        // Now that we have cleared it, we need to check the rrest of the table
        // Loop tbrough every PTE in this page
        for(unsigned int i = 0; i < PTE_CNT; ++i){

            // If any entry in the table is still valid, then we know there are still entries
            if(PTE_VALID(ptab[i])){

                // Return 1 when we know there are still entries
                return 1;
            }
        }

        // If passes the for loop, then we know its empty so return 2
        return 2;
    }

    // If its not a leaf mapping, then we need to go into the child page table and keep walking forward
    // Define the pointer to the child page-table page
    struct pte * child = pte_child(curr_pte);

    // Make the recurve call to the helper functon that tells it to go one down and store the returned resilt into a var to check with
    int result = ptab_remove_recursive_helper(child, level - 1, vpn, pp_out);

    // Now if the helper returns 0, then we know there is nothing more to do in the child
    if(result == 0){

        return 0;
    }
    
    // Now if the helper returns 2, that means it is empty which means we need to free and clear it
    if(result == 2){

        // free the child table page
        free_phys_page(child);

        // Clear the pte
        * curr_pte = null_pte();
    }

    // At this point we know the result is not 0 which means we need to check the current table
    // If ant entry is still valid, we do the same process as we did before
    // Loop tbrough every PTE in this page
    for(unsigned int i = 0; i < PTE_CNT; ++i){

        // If any entry in the table is still valid, then we know there are still entries
        if(PTE_VALID(ptab[i])){

            // Return 1 when we know there are still entries
            return 1;
        }
    }

    // If passes the for loop, then we know its empty so return 2
    return 2;
}

// Ptab_remove does pretty much the opposite of remove so the thge code is going to be very similar
// This one wont need the pp or the flags because we are removing the mapping
// We also are going to free and clean up the page tables that become empty as a result
static void * ptab_remove(struct pte * ptab, unsigned long vpn){

    // Call on the recusrive function for this
    // Set the page pointer to NULL as it nothing gets removed it stays NULL but gets set if it set
    void * pp = NULL;

    // Call on the recursive helper
    (void) ptab_remove_recursive_helper(ptab, ROOT_LEVEL, vpn, &pp);

    // Return the physical page pointer
    return pp;
}

// Ptab_adjust changes the access permmissions of the mapping, changing the RWXUG bits
static void ptab_adjust(struct pte * ptab, unsigned long vpn, int rwxug_flags){

    // Start with fetching the PTE that defines the mapping for this vpn
    struct pte * curr_pte = ptab_fetch(ptab, vpn);

    // If the PTE is unmapped, then return
    if(curr_pte == NULL){

        return;
    }

    // Only adjust leaf mappings because the permissions are applied to them
    if(!PTE_LEAF(* curr_pte)){

        // If not a leaf, then return
        return;
    }

    // At this point, its a valid leaf mapping which meeans we can start the adjusting
    // Start by saving the old flags as we are going to bitwise and with it to update
    uint8_t old_flags = curr_pte->flags;

    // Now we want to preserve all the bits except for the updated RWXUG bits which we are going to overwrite
    uint8_t non_flag_bits = (uint8_t)(old_flags & ~(PTE_R | PTE_W | PTE_X | PTE_U | PTE_G));

    // Now that we have the non important bits, we can bitwise or with the flags to set the flags to the update one
    curr_pte->flags = non_flag_bits | (uint8_t) rwxug_flags;
}
