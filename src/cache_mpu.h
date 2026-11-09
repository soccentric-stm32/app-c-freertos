/**
 * @file    cache_mpu.h
 * @brief   Cortex-M7 cache enable and the non-cacheable DMA memory window.
 * @details Declares the one-shot start-up sequence that turns the I-cache and
 *          the D-cache on and programs an MPU region over the 16 KB SRAM2
 *          window at 0x2007C000 as normal, non-cacheable memory.  Every DMA
 *          buffer in the application is linked into @c .dma_buffer inside that
 *          window, so the CPU and the DMA controller always observe the same
 *          bytes and no clean/invalidate maintenance is needed in the
 *          interrupt path (COM-002).  All memory outside the window keeps the
 *          architectural default attributes through the MPU background region
 *          (@c MPU_CTRL_PRIVDEFENA), which is what lets code, stacks, the
 *          FreeRTOS heap and the DTCM .noinit record work unchanged.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     DGEN-003 (caches on), COM-002 (DMA buffers)
 *
 * @note The ordering is inherited from the bootloader contract and
 *       DGEN-003/COM-002: BOOT hands over with the caches off, and
 *       @c main() enables them once, after @ref app_clock_init and before
 *       @ref app_uart3_init arms the first DMA channel.
 */

#ifndef CACHE_MPU_H
#define CACHE_MPU_H /**< Include guard for @ref cache_mpu.h. */

/**
 * @defgroup cache_mpu Cache and MPU configuration
 * @brief Cortex-M7 cache enable and the non-cacheable SRAM2 DMA window.
 * @details The Cortex-M7 core in the STM32F767 has separate I-cache and
 *          D-cache and is not coherent with the DMA controller.  A shared
 *          buffer must therefore either be maintained explicitly (clean
 *          before the peripheral reads it, invalidate after the peripheral
 *          writes it) or live in memory that bypasses the D-cache.  This
 *          module takes the second route:
 *
 *          - all DMA buffers are linked into @c .dma_buffer in SRAM2, the
 *            16 KB window 0x2007C000..0x2007FFFF (see @c ld/app.ld);
 *          - MPU region 6 marks that whole window as normal memory with
 *            TEX=1, C=0, B=0, S=0, that is non-cacheable and non-shareable;
 *          - the region is also XN (not executable) and grants full access,
 *            which matches the non-MPU FreeRTOS CM7 port used here, where
 *            tasks run privileged and only use normal loads and stores.
 *
 *          Cache maintenance on a DMA buffer is consequently never required,
 *          so the Comms task can read the USART3 receive ring while the DMA
 *          is still writing into it (COM-002).
 * @{
 */

/**
 * @brief Configure the non-cacheable DMA MPU region and enable the caches.
 * @details The function performs these steps in order:
 *          -# Disable the MPU and wait for the change to take effect
 *             (@c __DSB followed by @c __ISB).
 *          -# Program region 6 (DMA_MPU_REGION): base 0x2007C000, the start of
 *             SRAM2, and size 16 KB.  The RASR SIZE field holds
 *             log2(bytes) - 1, so DMA_MPU_SIZE_LOG2 is 13 for 2^(13+1) bytes.
 *             The base is naturally 16 KB aligned, as the MPU requires.
 *          -# Set the region attributes to XN=1 (not executable),
 *             AP=full access, TEX=1, C=0, B=0, S=0, that is normal,
 *             non-cacheable, non-shareable memory, and enable the region.
 *          -# Enable the MPU with @c MPU_CTRL_PRIVDEFENA, so privileged
 *             accesses outside region 6 fall back to the default memory map
 *             instead of faulting.
 *          -# Enable the I-cache and then the D-cache through
 *             @c SCB_EnableICache and @c SCB_EnableDCache.
 *
 *          Because the whole SRAM2 window is non-cacheable, DMA traffic in
 *          either direction needs no @c SCB_CleanDCache or
 *          @c SCB_InvalidateDCache call: the core never holds a stale or dirty
 *          line for an object in @c .dma_buffer.  Buffers elsewhere are still
 *          cacheable and remain the caller's responsibility.
 *
 * @pre Called from privileged thread mode, before @c vTaskStartScheduler and
 *      before any DMA channel is enabled.  @ref app_clock_init has already
 *      run, so the core is at its final 216 MHz (DGEN-003).
 * @post The I-cache and D-cache are enabled, the MPU is enabled with region 6
 *       active, and accesses to @c .dma_buffer bypass the D-cache.
 *
 *
 * @note Call this once.  The function writes the MPU control registers
 *       unconditionally and re-runs @c SCB_EnableDCache, which invalidates the
 *       D-cache by set/way; a second call after the caches are live would
 *       discard dirty lines.
 * @note The base address and 16 KB size are literals in @c cache_mpu.c and are
 *       not derived from the linker symbols.  The linker script asserts only
 *       that @c .dma_buffer fits in SRAM2, not that the SRAM2 origin still
 *       matches DMA_MPU_BASE.
 * @warning Only objects placed in the @c .dma_buffer section are covered by
 *          the non-cacheable region.  Passing a stack, @c .bss or heap buffer
 *          to a DMA driver reintroduces cache incoherence.
 * @warning Not ISR-safe and not thread-safe; it is a start-up-only call.
 *
 * @see app_clock_init, app_uart3_init
 */
void app_cache_mpu_init(void);

/** @} */

#endif /* CACHE_MPU_H */
