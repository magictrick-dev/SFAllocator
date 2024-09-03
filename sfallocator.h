// --- SF Allocator ------------------------------------------------------------
//
// Written by Chris DeJong, GitHub @ magictrickdev
//
//      SF allocator is a header-only general purpose allocator written in C and
//      is meant to be both transparent and performant. It also ships with common
//      memory operation utilities like memset, memcpy, and memzero. These libraries
//      will attempt to use the fastest available methods for the given architecture.
//      To disable this (and to use the defaults provided by the C-standard library),
//      define SF_USE_SSE_INTRINSICS to 0.
//
//      Drop this header file into your project and include it as desired.
//
//      To reserve a set amount of contiguous pages, use sf_init(). This function
//      silently fails if pages are already reserved.
//
// -----------------------------------------------------------------------------



#ifndef SFALLOCATOR_H
#define SFALLOCATOR_H
#include <stdint.h>
#include <stdbool.h>

void    sf_init(uint64_t reserve_size);
void*   sf_alloc(uint64_t size);
void    sf_free(void *ptr);
//void*   sf_alloc_ext(uint64_t size, bool touch_pages, bool zero_pages, bool fast);
//void*   sf_alloc_large(uint64_t size, uint64_t *size_out);

//void    sf_memzero(void *buffer, uint64_t size);
//void    sf_memset(void *buffer, uint64_t size, uint8_t byte);
//void    sf_memcopy(void *dst, uint64_t dst_size, void *src, uint64_t src_size);

#define SFA_BYTES(n)            (uint64_t)(n)
#define SFA_KILOBYTES(n)        (uint64_t)(1024 * SFA_BYTES(n))
#define SFA_MEGABYTES(n)        (uint64_t)(1024 * SFA_KILOBYTES(n))
#define SFA_GIGABYTES(n)        (uint64_t)(1024 * SFA_MEGABYTES(n))
#define SFA_TERABYTES(n)        (uint64_t)(1024 * SFA_GIGABYTES(n))

#ifndef SFA_ASSERT
#   include <assert.h>
#   define SFA_ASSERT(expr) assert((expr))
#endif

#define SFA_ASSERT_POINTER(ptr) SFA_ASSERT(ptr != NULL)
#define SFA_ASSERT_NOREACH()    SFA_ASSERT(!"Condition should not be reachable.")
#define SFA_ASSERT_NOIMPL()     SFA_ASSERT(!"Implementation not yet defined.")

#define SFA_ALLOCATION_ALIGNMENT_SIZE           (sizeof(uint64_t)*4)
#define SFA_ALLOCATION_MINIMUM_SIZE             (sizeof(uint64_t)*4)
#define SFA_ALLOCATION_MINIMUM_PAGES_PER_POOL   (4)
#define SFA_DEFAULT_INITIAL_POOL_SIZE           (SFA_KILOBYTES(256))

// -------------------------------------------------------------------------- \\
// *                                                                        * \\
//                                                                            \\
// *                                                                        * \\
//                                                                            \\
// *                         Implementation Below                           * \\
//                                                                            \\
// *                            Dragons Beyond                              * \\
//                                                                            \\
// *                                                                        * \\
//                                                                            \\
// *                                                                        * \\
// -------------------------------------------------------------------------- \\



// --- Internal API ------------------------------------------------------------
//
// Interfacing functions for the allocator's front-end API.
//
// Important Terminology
//
//      -   Allocation Descriptors:
//              These are placed at the "head" of every allocation. If you allocate
//              32 bytes, space is reserved for the descriptor + the request size +
//              any necessary padding and offsets to ensure proper data alignment.
//              These sizes are macro defined above and can be changed (not advised).
//
//              They contain pointers that refer to their adjacent elements. When
//              a block is freed, then it first coallesces with its adjacent nodes
//              or appends itself to the pool's free list.
//
//      -   Allocation Pool Descriptors:
//              These are placed at the "head" of every contiguous set of pages.
//              These descriptors contain information about the pool, the free list,
//              and other allocated pools.
//
//              When an allocation is made, pool descriptors are searched, finding
//              the best fit location for a given allocation.
//
// The first element in any free-list is always the tail. If the allocation reaches
// the end of the pool, a flag is set indicating that the first element of the free
// list is not the end of the pool and it is no longer optimal to allocate to. Further
// allocations to this pool are skipped until it regains a tail or is completely freed.
//
//      1.  Traversing linked lists isn't ideal, and for any given pool which lacks
//          a tail, it potentially means that it could be fragmented.
//      2.  Worst case is that the allocation that occupies the tail space is
//          the last thing to be freed.
//      3.  We optimize for best-fit via pools. We first find a pool that has
//          enough space to accomodate *then* we search for a place to fit the
//          allocation. When this fails, we immediately move to the next pool.
//          Finally, if all pools fail, then we check the skipped pools for
//          an appropriate spot. If that fails, then we generate a new pool.
//
// This process of searching may not be ideal for sections of code that may favor
// performance over space efficiency. The extended variants allow for fast allocations
// which only search for tails that can fit the allocation. This skips deep traversals
// to find the best place to put an allocation.
//

typedef struct sfa_allocation_descriptor    sfa_allocation_descriptor;
typedef struct sfa_pool_descriptor          sfa_pool_descriptor;
typedef struct sfa_state                    sfa_state;

inline void*        __sfa_virtual_alloc(void* offset, uint64_t size);
inline void         __sfa_virtual_free(void* ptr);
inline uint64_t     __sfa_virtual_size();
inline sfa_state*   __sfa_get_state();
inline uint64_t     __sfa_request_size_to_nearest_boundary(uint64_t size);
inline uint64_t     __sfa_request_size_to_minimum_pool_size(uint64_t size);
inline uint64_t     __sfa_request_size_to_minimum_alloc_size(uint64_t size);
inline sfa_pool_descriptor* __sfa_create_pool(uint64_t pool_size);

typedef struct sfa_state
{
    
    bool initialized;
    sfa_pool_descriptor *head_pool;
    sfa_pool_descriptor *tail_pool;

} sfa_state;

// Describes a block of memory within a pool.
typedef union sfa_allocation_flags
{
    
    uint64_t flags;

    struct
    {

        uint64_t is_occupied        : 1;    // Free blocks are marked 0, in-use is 1.
        uint64_t is_coallescable    : 1;    // If true, it is a large allocation.
        uint64_t                    : 62;   // Remaining bits.

    };

} sfa_allocation_flags;

// Placed at the front of every allocation.
typedef struct sfa_allocation_descriptor
{

    sfa_allocation_flags flags;
    sfa_allocation_descriptor *left_descriptor;
    sfa_allocation_descriptor *right_descriptor;
    sfa_pool_descriptor *parent_pool;
    uint64_t allocation_size;

} sfa_allocation_descriptor;

// Placed at the front of every pool of pages.
typedef struct sfa_pool_descriptor
{

    sfa_pool_descriptor        *next_pool;
    sfa_pool_descriptor        *prev_pool;

    sfa_allocation_descriptor  *free_head;
    sfa_allocation_descriptor  *free_tail;

    void       *memory_region;
    uint64_t    memory_region_size;
    uint64_t    memory_region_occupancy;
    bool        pool_is_large;

} sfa_pool_descriptor;

inline sfa_state*   
__sfa_get_state()
{

    static sfa_state state = {0};
    if (state.initialized == false)
    {
        state.head_pool = false;
        state.tail_pool = false;
        state.initialized = true;
    }

    return &state;

}

inline uint64_t     
__sfa_request_size_to_nearest_boundary(uint64_t size)
{

    uint64_t remainder = size % SFA_ALLOCATION_ALIGNMENT_SIZE;
    uint64_t boundary = size + remainder;
    return boundary;

}

inline uint64_t     
__sfa_request_size_to_minimum_pool_size(uint64_t size)
{

    uint64_t pool_size = __sfa_virtual_size();
    uint64_t pages_required = (size / pool_size) + (size % pool_size > 0);
    pages_required = (pages_required > SFA_ALLOCATION_MINIMUM_PAGES_PER_POOL) ? 
        pages_required : SFA_ALLOCATION_MINIMUM_PAGES_PER_POOL;
    return pages_required * pool_size;

}

inline uint64_t     
__sfa_request_size_to_minimum_alloc_size(uint64_t size)
{

    uint64_t size_required = (size > SFA_ALLOCATION_MINIMUM_SIZE) ?
        size : SFA_ALLOCATION_MINIMUM_SIZE;
    return size_required;

}

inline sfa_pool_descriptor* 
__sfa_create_pool(uint64_t pool_size)
{

    // Size and allocate. Virtual alloc can potentially fail, but only if the OS-call
    // is poorly formatted *OR* somehow we hit the virtual allocation limit.
    uint64_t actual_reserve_size = __sfa_request_size_to_minimum_pool_size(pool_size);
    void *alloc_buffer = __sfa_virtual_alloc(NULL, actual_reserve_size);
    SFA_ASSERT(alloc_buffer != NULL);

    // Create the pool, set the pool next and prev to NULL. The invokee of this
    // function is responsible for placing it in the list.
    sfa_pool_descriptor *pool = (sfa_pool_descriptor*)alloc_buffer;
    pool->next_pool = NULL;
    pool->prev_pool = NULL;

    // Defines the memory region that the pool descriptor refers to.
    uint64_t offset_size = __sfa_request_size_to_nearest_boundary(sizeof(sfa_pool_descriptor));
    uint8_t *memory_offset = (uint8_t*)alloc_buffer + offset_size;
    SFA_ASSERT(memory_offset % SFA_ALLOCATION_ALIGNMENT_SIZE);

    pool->memory_region             = memory_offset;
    pool->memory_region_size        = actual_reserve_size - offset_size;
    pool->memory_region_occupancy   = sizeof(sfa_allocation_descriptor);
    pool->pool_is_large             = false;

    // Finally, set the pool's initial free list.
    sfa_allocation_descriptor *free_tail = (sfa_allocation_descriptor*)memory_offset;
    free_tail->flags.is_occupied        = false;
    free_tail->flags.is_coallescable    = true;
    free_tail->left_descriptor          = NULL;
    free_tail->right_descriptor         = NULL;
    free_tail->parent_pool              = pool;
    free_tail->allocation_size          = pool->memory_region_size - sizeof(sfa_allocation_descriptor);

}

// --- Win32 Definitions -------------------------------------------------------
//
// The following definitions defined the required OS-specific internal API methods.
// Most of these functions are simply wrappers for the OS-equivelant calls and have
// little overhead (aside from the OS call itself).
//

#if defined (__WIN32)
#include <windows.h>

void* 
__sfa_virtual_alloc(void* offset, uint64_t size)
{

    void* buffer = VirtualAlloc(offset, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return buffer;

}

void 
__sfa_virtual_free(void** ptr)
{

    SFA_ASSERT_POINTER(*ptr);  
    VirtualFree(*ptr, 0, MEM_RELEASE);
    *ptr = NULL;

}

uint64_t
__sfa_virtual_size()
{

    // Cache this value, it never changes.
    static uint64_t page_granularity = 0;
    if (page_granularity == 0)
    {

        SYSTEM_INFO system_info = {0};
        GetSystemInfo(&system_info);
        page_granularity = system_info.dwAllocationGranularity;

    }

    return page_granularity;

}

#endif

// --- External API ------------------------------------------------------------
//
// Implementations of the external API functions.
//

void*   
sf_init(uint64_t reserve_size)
{

    sfa_state *state = __sfa_get_state();
    sfa_pool_descriptor *pool = __sfa_create_pool(reserve_size);
    
}

void*   
sf_alloc(uint64_t size)
{

    sfa_state *state = __sfa_get_state();
    if (state->head_pool == NULL) sf_init(SFA_DEFAULT_INITIAL_POOL_SIZE);

}

void    
sf_free(void *ptr)
{

}

#endif
