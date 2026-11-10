/**
 * @file    flatcc_port.c
 * @brief   Fixed-arena first-fit allocator that backs the flatcc runtime.
 * @details CMakeLists.txt redirects every flatcc allocation hook (FLATCC_ALLOC,
 *          FLATCC_FREE, FLATCC_REALLOC, FLATCC_CALLOC, FLATCC_ALIGNED_ALLOC,
 *          FLATCC_ALIGNED_FREE, FLATCC_EMITTER_ALLOC and FLATCC_EMITTER_FREE)
 *          to the entry points below, so the builder, verifier and emitter
 *          take their buffers from one static arena in .bss instead of the C
 *          heap (DRT-004, DCON-002).  As a result malloc()/free() are never
 *          linked into the image.
 *
 *          The arena is a single @ref flatcc_arena_t object.  It is carved
 *          into 16-byte aligned blocks, each preceded by a @ref
 *          flatcc_block_t header, and kept in one singly linked list ordered
 *          by address.  Allocation is first-fit with tail splitting; freeing
 *          marks a block reusable and coalesces it with a free predecessor and
 *          then with free successors.  There is no lock and no metadata outside
 *          the arena: the runtime is expected to serialize its own use.
 *
 *          The public contract of each hook is documented in @ref
 *          flatcc_port.h; this file repeats it together with the
 *          implementation-specific details (alignment, split threshold,
 *          coalescing and failure behaviour).
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     DRT-004 (no allocation after start-up), DCON-002
 * @ingroup flatcc_port
 */

#include <string.h>
#include "protocol/flatcc_port.h"

/**
 * @brief Fallback size of the static flatcc arena in bytes.
 * @details CMakeLists.txt normally defines APP_FLATCC_HEAP_SIZE from the
 *          APP_FLATCC_HEAP_KB cache variable (12 KiB by default).  The 16 KiB
 *          value below applies only to builds that compile this file without
 *          that definition.
 * @note The size must be a multiple of @ref FLATCC_ALIGNMENT, and must be at
 *       least @c sizeof(flatcc_block_t) plus one payload byte, otherwise the
 *       initial block would underflow in flatcc_heap_init().
 */
#ifndef APP_FLATCC_HEAP_SIZE
#define APP_FLATCC_HEAP_SIZE (16U * 1024U)
#endif

/**
 * @brief Arena block granularity in bytes.
 * @details Payload requests are rounded up to this quantum, blocks are split
 *          on quantum boundaries and every returned payload is aligned to it,
 *          so the arena satisfies any alignment request up to 16 bytes.
 * @warning This value must equal @c sizeof(flatcc_block_t).  It is the distance
 *          between consecutive payload starts, and changing one without the
 *          other would break the guaranteed alignment.
 */
#define FLATCC_ALIGNMENT 16U

/**
 * @brief Header that precedes every payload in the arena.
 * @details The header is 16 bytes on the 32-bit target, matching @ref
 *          FLATCC_ALIGNMENT.  Headers live inside the arena itself, so the
 *          usable bytes are the arena size minus one header per block; there
 *          is no out-of-band bookkeeping and no per-allocation cost outside the
 *          arena.
 * @note @c pad carries no information; it only rounds the header up to the
 *       block alignment.
 * @warning The layout is private to this file and is not a wire or persistent
 *          format.  Do not reorder the fields without revisiting the split
 *          logic in app_flatcc_alloc().
 */
typedef struct flatcc_block
{
    size_t               size; /**< Payload size in bytes, header excluded. */
    struct flatcc_block *next; /**< Next block by address; NULL for the last. */
    uint32_t             free; /**< Non-zero while this payload is available. */
    uint32_t             pad;  /**< Unused; keeps the header 16 bytes long. */
} __attribute__((aligned(FLATCC_ALIGNMENT))) flatcc_block_t;

/**
 * @brief Backing storage of the one static arena.
 * @details Only @c bytes is used; @c align and @c pointer exist purely to force
 *          the alignment of the union to the worst-case scalar and pointer
 *          alignment.  The object lives in .bss, so it is zero-filled by the C
 *          start-up code before main() runs, which is what leaves @ref
 *          flatcc_head NULL until the first allocation.
 * @note @c bytes has no attribute of its own; the @c aligned attribute on the
 *       union applies to the whole object and therefore to @c bytes.
 */
typedef union
{
    long double   align;                       /**< Forces scalar alignment of the union. */
    void         *pointer;                     /**< Forces pointer alignment of the union. */
    unsigned char bytes[APP_FLATCC_HEAP_SIZE]; /**< Arena storage; only used member. */
} __attribute__((aligned(FLATCC_ALIGNMENT))) flatcc_arena_t;

/** The single arena; every payload handed out points into @c bytes. */
static flatcc_arena_t flatcc_arena;

/**
 * @brief Head of the block list, in ascending address order.
 * @details NULL until flatcc_heap_init() has run at least once.  After
 *          initialization the list always covers the whole arena exactly once:
 *          a block is either in use or free, and app_flatcc_free() merges every
 *          free neighbour so two adjacent free blocks never stay separate.
 * @note The list is singly linked and has no tail pointer; free() therefore
 *       walks it from the head to find a block's predecessor.
 */
static flatcc_block_t *flatcc_head;

/**
 * @brief Round a payload size up to the arena block alignment.
 * @param[in] n Requested payload size in bytes.
 *
 * @return @p n rounded up to the next multiple of @ref FLATCC_ALIGNMENT.
 * @retval 0 @p n is 0.
 * @retval SIZE_MAX @p n is too large to round without wrapping.
 *
 * @note A request that would overflow the addition is saturated to
 *       @c SIZE_MAX, which is larger than the whole arena and is therefore
 *       rejected by @ref app_flatcc_alloc instead of being rounded down to a
 *       small block.
 * @see app_flatcc_alloc
 */
static size_t flatcc_align(size_t n)
{
    if(n > (SIZE_MAX - (FLATCC_ALIGNMENT - 1U)))
    {
        return SIZE_MAX;
    }
    return (n + (FLATCC_ALIGNMENT - 1U)) & ~(size_t)(FLATCC_ALIGNMENT - 1U);
}

/**
 * @brief Lazily turn the whole arena into a single free block.
 * @details Idempotent: the body executes only while @ref flatcc_head is NULL,
 *          so later calls cost one pointer comparison.  On the first call the
 *          head header is placed at the start of flatcc_arena.bytes and owns
 *          every remaining byte of the arena.
 *
 * @pre The C start-up code has zeroed .bss, so @ref flatcc_head is NULL and
 *      @ref flatcc_arena reads as zero.
 * @post @ref flatcc_head points to one free block whose payload spans the arena
 *       minus a single header.  The @c pad field is left as it was (zero).
 * @post The arena can only ever be initialized once; there is no reset hook, so
 *       the free list survives for the lifetime of the firmware.
 *
 * @note Called from app_flatcc_alloc() on every allocation, which is why it is
 *       written as a cheap guarded check rather than a start-up routine.
 * @warning Not reentrant and not thread-safe: two concurrent first calls would
 *          both reinitialize the list head.
 * @see app_flatcc_alloc
 * @see flatcc_head
 */
static void flatcc_heap_init(void)
{
    if(flatcc_head == NULL)
    {
        flatcc_head       = (flatcc_block_t *)flatcc_arena.bytes;
        flatcc_head->size = APP_FLATCC_HEAP_SIZE - sizeof(flatcc_block_t);
        flatcc_head->next = NULL;
        flatcc_head->free = 1U;
    }
}

/**
 * @brief Allocate a block from the fixed flatcc arena.
 * @details Initializes the arena on first use, rejects a zero request, rounds
 *          @p size up with flatcc_align(), then scans the block list from the
 *          head and takes the first free block that is large enough (first-fit,
 *          O(number of blocks)).  When the remainder of that block can hold
 *          another header plus at least @ref FLATCC_ALIGNMENT payload bytes, the
 *          block is split: a new header is placed @p size bytes after the
 *          current payload start, the tail keeps the rest of the space and
 *          links to the old successor, and the original block is trimmed.  If
 *          the remainder is smaller than @c sizeof(flatcc_block_t) +
 *          FLATCC_ALIGNMENT (32 bytes) no split happens and the whole block is
 *          consumed, wasting at most 31 bytes.
 *
 *          The returned pointer is the byte after the header, hence
 *          @ref FLATCC_ALIGNMENT aligned, because the header is 16 bytes and
 *          split offsets are multiples of 16.  The memory is not cleared.
 *
 * @param[in] size Requested payload size in bytes; rounded up to a multiple of
 *                 16.
 *
 * @return Pointer to the payload, or NULL when the request cannot be served.
 * @retval NULL @p size is zero.
 * @retval NULL every block is either in use or smaller than the rounded
 *              request.
 *
 * @pre None; the arena is initialized on the first call.
 * @post On success the block is marked in use and linked into the list exactly
 *       once.
 * @post No C heap memory is touched and no error is latched; failure is
 *       reported only through the return value.
 *
 * @note The search is first-fit, so earlier small free blocks are reused before
 *       later large ones; fragmentation is bounded only by the coalescing in
 *       app_flatcc_free().
 * @warning Not thread-safe and not ISR-safe.  flatcc callers run on the Comms
 *          task (the builder is initialized from comms_task_create() before the
 *          scheduler starts), so no lock is taken.
 * @see app_flatcc_calloc
 * @see app_flatcc_free
 * @see app_flatcc_realloc
 */
void *app_flatcc_alloc(size_t size)
{
    flatcc_heap_init();
    if(size == 0U)
    {
        return NULL;
    }
    size = flatcc_align(size);

    for(flatcc_block_t *block = flatcc_head; block != NULL; block = block->next)
    {
        if(block->free == 0U || block->size < size)
        {
            continue;
        }
        /* Split when the tail can hold a header and 16 payload bytes. */
        if((block->size - size) >= (sizeof(flatcc_block_t) + FLATCC_ALIGNMENT))
        {
            flatcc_block_t *tail = (flatcc_block_t *)((unsigned char *)block + sizeof(flatcc_block_t) + size);
            tail->size           = block->size - size - sizeof(flatcc_block_t);
            tail->next           = block->next;
            tail->free           = 1U;
            block->next          = tail;
            block->size          = size;
        }
        block->free = 0U;
        return (unsigned char *)block + sizeof(flatcc_block_t);
    }
    return NULL;
}

/**
 * @brief Return a block to the fixed flatcc arena.
 * @details A NULL argument is ignored.  Otherwise the header is recovered at
 *          @p ptr minus @c sizeof(flatcc_block_t), the block is marked free, the
 *          list is walked from @ref flatcc_head to find the immediate
 *          predecessor, and if that predecessor is also free the two blocks are
 *          merged (predecessor payload grows by one header plus the freed
 *          payload, predecessor adopts the freed block's successor).  Finally
 *          every free successor is merged into the resulting block, so adjacent
 *          free blocks are always collapsed into one.
 *
 *          The function never returns memory to the C heap, never shrinks the
 *          reported arena size and cannot fail.
 *
 * @param[in] ptr Payload pointer returned by app_flatcc_alloc(),
 *                app_flatcc_realloc(), app_flatcc_calloc() or
 *                app_flatcc_aligned_alloc(), or NULL.
 *
 * @pre @p ptr is NULL or points to a payload that is currently in use in the
 *      arena.
 * @post The block is free, possibly merged with its free neighbours, and may be
 *       handed out again by app_flatcc_alloc().
 * @post With @p ptr NULL the list is untouched.
 *
 * @note The predecessor lookup is O(number of blocks) because the list is
 *       singly linked and has no prev pointer.
 * @warning No validation is performed: @p ptr is neither range-checked against
 *          the arena nor checked for a pointer into the middle of a payload.
 *          A foreign pointer still silently corrupts the block list.
 * @note  An immediately repeated free of a block that is still free is ignored.
 *        A pointer that was already coalesced into a neighbour, or that never
 *        belonged to the arena, is no longer a valid header and must not be
 *        reused; the guard cannot detect it.
 * @warning Not thread-safe and not ISR-safe; it mutates the shared list.
 * @see app_flatcc_alloc
 * @see app_flatcc_realloc
 * @see app_flatcc_aligned_free
 */
void app_flatcc_free(void *ptr)
{
    if(ptr == NULL)
    {
        return;
    }
    flatcc_block_t *block = (flatcc_block_t *)((unsigned char *)ptr - sizeof(flatcc_block_t));
    if(block->free != 0U)
    {
        return; /* Already free: ignore an immediately repeated free. */
    }
    block->free = 1U;

    /* Coalesce with a free predecessor, then with free followers. */
    flatcc_block_t *prev = flatcc_head;
    while(prev != NULL && prev->next != block)
    {
        prev = prev->next;
    }
    if(prev != NULL && prev->free != 0U)
    {
        prev->size += sizeof(flatcc_block_t) + block->size;
        prev->next = block->next;
        block      = prev;
    }
    while(block->next != NULL && block->next->free != 0U)
    {
        flatcc_block_t *next = block->next;
        block->size += sizeof(flatcc_block_t) + next->size;
        block->next = next->next;
    }
}

/**
 * @brief Resize an arena allocation while preserving its contents.
 * @details Dispatches on the arguments first: @p ptr NULL behaves like
 *          app_flatcc_alloc(@p size), and @p size 0 frees @p ptr and returns
 *          NULL.  Otherwise the block header is recovered from @p ptr and the
 *          aligned request is compared with the block's current payload size.
 *          If the block already fits, @p ptr is returned unchanged (a shrink
 *          does not split the block).  Otherwise a new block is allocated, the
 *          old payload is copied with memcpy() using the old payload size, and
 *          the old block is freed.
 *
 * @param[in] ptr  Existing arena payload, or NULL.
 * @param[in] size New requested payload size in bytes; rounded up to 16.
 *
 * @return Pointer to the payload of the resized block, or NULL on failure.
 * @retval NULL @p size is zero; @p ptr has already been freed.
 * @retval NULL no free block is large enough; @p ptr and its contents are
 *              unchanged and still owned by the caller.
 *
 * @pre @p ptr is NULL or a live arena payload.
 * @post On success the old pointer must no longer be used and the returned
 *       pointer owns the data.
 * @post On failure nothing changed.
 *
 * @note The copy length is the old payload size, which may exceed the size the
 *       caller originally requested, because alloc() rounds requests up.
 * @note A grow always allocates a new block first, so it needs temporary space
 *       for both the old and the new payload; a copy within one block is never
 *       attempted.
 * @warning Not thread-safe and not ISR-safe; see app_flatcc_alloc().
 * @see app_flatcc_alloc
 * @see app_flatcc_free
 */
void *app_flatcc_realloc(void *ptr, size_t size)
{
    if(ptr == NULL)
    {
        return app_flatcc_alloc(size);
    }
    if(size == 0U)
    {
        app_flatcc_free(ptr);
        return NULL;
    }

    flatcc_block_t *block = (flatcc_block_t *)((unsigned char *)ptr - sizeof(flatcc_block_t));
    if(block->size >= flatcc_align(size))
    {
        return ptr;
    }

    void *new_ptr = app_flatcc_alloc(size);
    if(new_ptr == NULL)
    {
        return NULL;
    }
    memcpy(new_ptr, ptr, block->size);
    app_flatcc_free(ptr);
    return new_ptr;
}

/**
 * @brief Allocate a zero-filled array from the fixed flatcc arena.
 * @details Rejects a multiplication that would overflow @c size_t, then
 *          allocates @p count * @p size bytes through app_flatcc_alloc() and
 *          clears exactly that many bytes with memset().
 *
 * @param[in] count Number of elements.
 * @param[in] size  Size of one element in bytes.
 *
 * @return Pointer to the zeroed payload, or NULL on failure.
 * @retval NULL @p count * @p size overflows @c size_t, in which case the arena
 *              is left untouched.
 * @retval NULL the product is zero, because app_flatcc_alloc(0) returns NULL.
 * @retval NULL the arena has no block large enough.
 *
 * @pre None.
 * @post On success every byte of the requested payload is zero and the block is
 *       in use.
 *
 * @note The overflow test skips @p count == 0 so that a zero count cannot
 *       divide by zero; it can never reject a valid product.
 * @warning Not thread-safe and not ISR-safe; see app_flatcc_alloc().
 * @see app_flatcc_alloc
 * @see app_flatcc_free
 */
void *app_flatcc_calloc(size_t count, size_t size)
{
    if(count != 0U && size > (SIZE_MAX / count))
    {
        return NULL;
    }
    size_t total = count * size;
    void  *ptr   = app_flatcc_alloc(total);
    if(ptr != NULL)
    {
        memset(ptr, 0, total);
    }
    return ptr;
}

/**
 * @brief Allocate storage with the flatcc requested alignment.
 * @details The arena only ever returns @ref FLATCC_ALIGNMENT aligned payloads,
 *          so @p alignment is ignored (cast to void to keep -Wextra quiet) and
 *          the request is forwarded to app_flatcc_alloc().  flatcc uses this
 *          hook for its aligned allocation paths.
 *
 * @param[in] alignment Requested alignment in bytes; ignored.
 * @param[in] size      Requested payload size in bytes; rounded up to 16.
 *
 * @return Pointer to a 16-byte aligned payload, or NULL when the request
 *         cannot be served (see app_flatcc_alloc()).
 *
 * @note Because @p alignment is not used, a request whose @p size is not a
 *       multiple of @p alignment succeeds here even though C11 aligned_alloc()
 *       would reject it; the returned block is still correctly aligned for any
 *       power-of-two alignment up to 16.
 * @warning An alignment greater than 16 is neither rejected nor honoured; the
 *          result is under-aligned for that request.  Do not rely on this hook
 *          for larger alignments in this port.
 * @warning Not thread-safe and not ISR-safe; see app_flatcc_alloc().
 * @see app_flatcc_aligned_free
 * @see app_flatcc_alloc
 */
void *app_flatcc_aligned_alloc(size_t alignment, size_t size)
{
    (void)alignment; /* the arena always returns 16-byte aligned blocks */
    return app_flatcc_alloc(size);
}

/**
 * @brief Free storage obtained from app_flatcc_aligned_alloc().
 * @details Aligned allocation returns an ordinary arena payload, so this is a
 *          direct forward to app_flatcc_free(), which also accepts NULL.
 *
 * @param[in] ptr Pointer returned by app_flatcc_aligned_alloc(), or NULL.
 *
 * @pre @p ptr is NULL or a live arena payload.
 * @post The block is free and may be reused or coalesced.
 *
 * @note Any arena payload may be passed here, not only pointers that came from
 *       app_flatcc_aligned_alloc(), because both paths return the same block
 *       layout.
 * @warning Not thread-safe and not ISR-safe; see app_flatcc_free().
 * @see app_flatcc_aligned_alloc
 * @see app_flatcc_free
 */
void app_flatcc_aligned_free(void *ptr) { app_flatcc_free(ptr); }
