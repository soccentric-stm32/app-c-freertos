/**
 * @file    flatcc_port.h
 * @brief   Allocation hooks that bind the flatcc runtime to a static arena.
 * @details The flatcc runtime is built with FLATCC_ALLOC, FLATCC_FREE,
 *          FLATCC_REALLOC, FLATCC_CALLOC, FLATCC_ALIGNED_ALLOC,
 *          FLATCC_ALIGNED_FREE, FLATCC_EMITTER_ALLOC and FLATCC_EMITTER_FREE
 *          redirected to the functions below.  All of them use one fixed
 *          arena in .bss, so the runtime performs no heap allocation and
 *          malloc/free are never linked (DRT-004).  The arena is sized by
 *          APP_FLATCC_HEAP_SIZE, which CMake derives from APP_FLATCC_HEAP_KB;
 *          flatcc_port.c supplies a fallback when the macro is absent.
 *          CMakeLists.txt force-includes this header into every C translation
 *          unit with -include so each expansion of the runtime hooks sees
 *          these prototypes.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     DRT-004 (no allocation after start-up), DCON-002 (flatcc runtime)
 * @ingroup flatcc_port
 */

#ifndef FLATCC_PORT_H
/**
 * @brief Include guard for @ref flatcc_port.h.
 * @details Defined after the first inclusion.  The header only declares
 *          functions, so the guard just avoids duplicate prototypes.
 */
#define FLATCC_PORT_H

#include <stddef.h>
#include <stdint.h>

/**
 * @defgroup flatcc_port flatcc static-arena allocator
 * @brief C allocation entry points backed by a fixed arena in .bss.
 * @details CMake redirects every flatcc runtime allocation hook to these
 *          functions, so the builder, verifier and emitter never call the C
 *          heap.  The arena is one static object; allocation is first-fit with
 *          16-byte blocks, and free blocks are coalesced.  The hooks are not
 *          locked and are used only from the Comms task (DRT-004).
 * @note The arena size comes from APP_FLATCC_HEAP_SIZE.  CMake defines it
 *       from APP_FLATCC_HEAP_KB (the cache default is 12 KiB), while
 *       flatcc_port.c falls back to 16 KiB if the macro is not defined.
 * @warning Do not mix these pointers with malloc()/free().
 * @{
 */

/**
 * @brief Allocate a block from the fixed flatcc arena.
 * @details Lazily initializes the arena on first use, rounds @p size up to
 *          the 16-byte block alignment, and returns the first free block that
 *          is large enough.  A block whose tail can hold another header plus
 *          at least 16 payload bytes is split; otherwise the whole block is
 *          consumed.  No heap allocation is performed at any point.
 *
 * @param[in] size Requested payload size in bytes.
 *
 * @return Pointer to the payload, or NULL when the request cannot be served.
 * @retval NULL @p size is zero.
 * @retval NULL no free block is large enough.
 *
 * @pre None; the arena is initialized on the first call.
 * @post On success the returned block is marked in use until it is freed.
 * @note The returned memory is not zeroed; use app_flatcc_calloc() when
 *       zero-initialized storage is required.
 * @warning Not thread-safe.  The runtime serializes its builder/verifier use
 *          on the Comms task, so no lock is taken (see flatcc_port.c).
 * @see app_flatcc_free
 * @see app_flatcc_calloc
 */
void *app_flatcc_alloc(size_t size);

/**
 * @brief Return a block to the fixed flatcc arena.
 * @details No-op when @p ptr is NULL.  The block is marked free, merged with a
 *          free immediate predecessor, and then merged with every free
 *          successor in the arena list.  Nothing is returned to the C heap.
 *
 * @param[in] ptr Payload pointer returned by one of the arena allocators, or
 *                NULL.
 *
 * @pre @p ptr is NULL or points to a live arena payload.
 * @post The block is free and may be reused or merged.
 * @note Passing the same block twice violates the @p ptr precondition and can
 *       corrupt the free list.
 * @warning Not thread-safe and may not be called from an ISR while the Comms
 *          task is using the arena.
 * @see app_flatcc_alloc
 */
void app_flatcc_free(void *ptr);

/**
 * @brief Resize an arena allocation while preserving its contents.
 * @details With @p ptr NULL this is app_flatcc_alloc(@p size).  With @p size
 *          zero the block is freed and NULL is returned.  If the existing
 *          block already fits the aligned request it is returned unchanged;
 *          otherwise a new block is allocated, the old payload is copied, and
 *          the old block is freed.
 *
 * @param[in] ptr  Existing arena payload, or NULL.
 * @param[in] size New requested payload size in bytes.
 *
 * @return Pointer to the resized payload, or NULL on failure.
 * @retval NULL @p size is zero, after freeing @p ptr.
 * @retval NULL the arena has no block large enough; @p ptr is unchanged.
 *
 * @pre @p ptr is NULL or points to a live arena payload.
 * @post On success the old pointer is invalid and the new one owns the data.
 * @note A shrink returns the same pointer without splitting the block.
 *       Bytes beyond the old payload in a grown block are uninitialized.
 * @warning Not thread-safe; see app_flatcc_alloc().
 * @see app_flatcc_alloc
 * @see app_flatcc_free
 */
void *app_flatcc_realloc(void *ptr, size_t size);

/**
 * @brief Allocate a zero-filled array from the fixed flatcc arena.
 * @details Rejects a multiplication that would overflow @c size_t, allocates
 *          @p count * @p size bytes with app_flatcc_alloc(), and clears the
 *          result with memset().
 *
 * @param[in] count Number of elements.
 * @param[in] size  Size of one element in bytes.
 *
 * @return Pointer to the zero-filled payload, or NULL on failure.
 * @retval NULL @p count * @p size overflows @c size_t.
 * @retval NULL the arena has no block large enough, including when either
 *              argument makes the total zero.
 *
 * @post On success every byte in the requested payload is zero.
 * @warning Not thread-safe; see app_flatcc_alloc().
 * @see app_flatcc_alloc
 */
void *app_flatcc_calloc(size_t count, size_t size);

/**
 * @brief Allocate storage with the flatcc requested alignment.
 * @details The arena only ever returns 16-byte aligned payloads, so
 *          @p alignment is ignored and the request is forwarded to
 *          app_flatcc_alloc().  flatcc uses this hook for its aligned
 *          allocation paths.
 *
 * @param[in] alignment Requested alignment in bytes.  Ignored.
 * @param[in] size      Requested payload size in bytes.
 *
 * @return Pointer to a 16-byte aligned payload, or NULL on failure.
 *
 * @note The returned pointer is suitable for alignment values up to 16.
 * @warning A request for alignment greater than 16 is not rejected and
 *          returns an under-aligned pointer.  Do not rely on this hook for
 *          larger alignments in this port.
 * @see app_flatcc_alloc
 * @see app_flatcc_aligned_free
 */
void *app_flatcc_aligned_alloc(size_t alignment, size_t size);

/**
 * @brief Free storage from app_flatcc_aligned_alloc().
 * @details Because aligned allocation returns a normal arena payload, this is
 *          a direct forward to app_flatcc_free().  NULL is accepted.
 *
 * @param[in] ptr Pointer returned by app_flatcc_aligned_alloc(), or NULL.
 *
 * @pre @p ptr is NULL or a live arena payload.
 * @post The block is returned to the free list.
 * @warning Not thread-safe; see app_flatcc_free().
 * @see app_flatcc_aligned_alloc
 * @see app_flatcc_free
 */
void app_flatcc_aligned_free(void *ptr);

/** @} */

#endif /* FLATCC_PORT_H */
